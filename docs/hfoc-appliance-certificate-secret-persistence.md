# HFOC Appliance Certificate And Secret Persistence

**Date:** 2026-09-05
**Target:** ITEM-325 of
`docs/superpowers/plans/2026-09-03-hfoc-appliance-platform.md`
**Base SHA:** `a28d8dba4b744d36616a75a3c3973b49cff4a502`
**Status:** Completed by ITEM-325

## Purpose

This document is the authoritative ITEM-325 certificate/secret persistence
policy boundary. ITEM-323 named the `certificate` and `secret` persistent
namespaces, ITEM-324 published the configuration record and recovery policy,
and ITEM-322 keeps runtime-usable private-key material volatile and zeroized.
ITEM-325 publishes the exact certificate/secret record families, an at-rest
record-value gate, and an HTTPS certificate-dependency readiness contract so
future production state loading cannot accidentally persist raw secret bytes
or start the HTTPS service before a persistent certificate/key pair is
available.

The item is a pre-publication policy and integration boundary. It performs no
storage I/O, no cryptographic wrapping, no boot change, and no protected HTTPS
route change. ITEM-334/335 own production boot storage discovery and reboot
acceptance.

## Record Catalogue

The canonical certificate/secret persistence catalogue is:

| Kind | Name | Namespace | Prefix | Handling |
| --- | --- | --- | --- | --- |
| 1 | `certificate.https` | `certificate` | `v1:certificate.https=` | raw certificate |
| 2 | `secret.admin.credential` | `secret` | `v1:secret.admin.credential=` | fingerprint only |
| 3 | `secret.https.private_key.digest` | `secret` | `v1:secret.https.private_key.digest=` | digest only |
| 4 | `secret.https.private_key.wrapped` | `secret` | `v1:secret.https.private_key.wrapped=` | wrapped only |

Every prefix must be followed by at least one value byte. The full record may
retain an optional single trailing newline when present in a journal block;
the classifier accepts the same bounded record line without a trailing
newline.

## At-Rest Record Policy

`appliance_secret_persistence_validate_value()` is the deny-by-default
pre-publication value gate:

- `certificate.https` accepts a nonempty bounded DER certificate chain and
  rejects all-zero or over-limit values.
- `secret.admin.credential` accepts exactly 64 lowercase hexadecimal
  characters. It never accepts raw credential bytes.
- `secret.https.private_key.digest` accepts exactly 64 lowercase hexadecimal
  characters for digest/rotation metadata.
- `secret.https.private_key.wrapped` accepts a nonempty bounded opaque wrapped
  blob and rejects all-zero or over-limit values.

There is no canonical record key for a raw private-key DER or PKCS record.
The wrapped record is an opaque gate for a future approved wrapping provider;
this policy boundary does not invent or weaken a wrapping algorithm.

## HTTPS Readiness

`appliance_secret_persistence_https_readiness()` consumes a caller-owned
state with certificate and wrapped-private-key publication booleans. The
certificate dependency is ready only when both are published. The function
publishes the existing `HTTPS_SERVICE_DEPENDENCY_CERTIFICATE_READY` bit,
which a future service loader composes with the unchanged network and entropy
bits in `https_service_readiness`.

## Boundary Contract

`appliance_secret_persistence` is allocation-free, read-only,
deny-by-default, and free of storage I/O, diagnostic output, audit writes,
blocking, locking, and interrupt-context access. The immutable catalogue has
no initialization step. Failed lookups and validation never publish an
ambiguous kind, namespace, version, handling policy, or HTTPS readiness.
No status or string contains a secret, fingerprint, digest, or wrapped-blob
byte.

## Future Ownership

- ITEM-326/ITEM-327 (completed): unified health and bounded
  diagnostics/logs in `docs/hfoc-appliance-health-model.md` and
  `docs/hfoc-appliance-runtime-diagnostics-logs.md`.
- ITEM-329 onward and recovery items: update, recovery, rollback, and reset
  state.
- ITEM-334/335: production boot storage discovery, approved wrapping
  provider integration if later approved, and reboot acceptance.

This item does not activate those owners or change the current normal-boot
path.

## Non-Goals

- No storage I/O, data-region changes, journal changes, or record migration.
- No unified production state loader or raw private-key persistence.
- No new cryptographic wrapping provider, certificate import, or TLS behavior.
- No HTTPS route, authentication, authorization, or management API change.
- No QEMU proof, because this item changes no observable boot behavior.
- No release version change.
