# HFOC Appliance Persistent Storage Namespaces

**Date:** 2026-09-05
**Target:** ITEM-323 of
`docs/superpowers/plans/2026-09-03-hfoc-appliance-platform.md`
**Base SHA:** `15fdc0919ea8f8fb6d953e892469950c98c0c1ed`
**Status:** Completed by ITEM-323

## Purpose

This document is the authoritative ITEM-323 persistent-storage-namespace
boundary. HFOC OS has no general writable filesystem and uses a checksummed
configuration journal plus bounded block-storage proofs. ITEM-323 names the
six persistent namespace domains that later roadmap items must use and
provides an immutable registry plus record-key classifier so unknown
namespace families fail closed.

The item is intentionally definitional: it publishes namespaces and
sensitivity labels, but performs no storage I/O. ITEM-324 completed the
configuration persistence and integrity policy boundary, ITEM-325 completed
the certificate/secret persistence policy, ITEM-326 completed the unified
appliance health model, ITEM-327 completed bounded runtime diagnostics/logs, and
recovery metadata belongs to the recovery roadmap.

## Namespace Table

The canonical inventory is:

| ID | Name | Record prefixes | Sensitivity |
| --- | --- | --- | --- |
| 1 | `system.config` | `v1:system.`, `v1:network.` | PRIVATE |
| 2 | `service.data` | `v1:service.` | PRIVATE |
| 3 | `certificate` | `v1:certificate.` | PRIVATE |
| 4 | `secret` | `v1:secret.` | SECRET |
| 5 | `diagnostic` | `v1:diagnostic.` | PRIVATE |
| 6 | `recovery.metadata` | `v1:recovery.` | PRIVATE |

System configuration includes both existing `v1:system.*` and `v1:network.*`
record families. Service data covers the existing `v1:service.*` enablement
records and future application/service persistent state. Certificate, secret,
diagnostic, and recovery prefixes are reserved for focused later items.

## Sensitivity

`GENERAL` data may be surfaced in public diagnostic views. `PRIVATE` data
requires bounded privileged/operational handling and must not be exposed raw
in public diagnostics. `SECRET` data requires redaction, zeroization,
secret-gate, and at-rest protection contracts.

No namespace is `GENERAL` today. System configuration, service data,
certificates, diagnostics, and recovery metadata are all `PRIVATE`; secret
material is `SECRET`. Later roadmap items may publish only sanitized views,
never raw namespace contents.

## Registry Contract

`appliance_persistent_storage_namespace` is the immutable kernel-owned
catalogue under `kernel/`. It exposes:

- `appliance_persistent_storage_namespace_count()` returns six.
- `appliance_persistent_storage_namespace_catalogue_at()` returns an exact
  descriptor by zero-based position.
- `appliance_persistent_storage_namespace_describe()` returns an exact
  descriptor by namespace ID.
- `appliance_persistent_storage_namespace_classify_record_key()` matches a
  bounded record key against canonical prefixes and returns the owning
  namespace.
- exact namespace-ID, sensitivity, and status string helpers.

The registry performs no allocation, no storage I/O, no diagnostic output,
and no state mutation. Unknown IDs, unknown prefixes, null input, empty keys,
and bare prefixes without a record byte are rejected with exact status codes.

## Future Ownership

- ITEM-324 (completed): configuration persistence and integrity policy.
- ITEM-325 (completed): certificate/secret persistence policy and HTTPS
  certificate-dependency readiness in
  `docs/hfoc-appliance-certificate-secret-persistence.md`.
- ITEM-326 (completed): unified appliance health model in
  `docs/hfoc-appliance-health-model.md`.
- ITEM-327 (completed): bounded runtime diagnostics/logs in
  `docs/hfoc-appliance-runtime-diagnostics-logs.md`.
- ITEM-329 onward and recovery items: update, recovery, rollback, and reset
  state.
- ITEM-334/335: production boot storage discovery and reboot acceptance.

This item does not activate any of those future owners or change the current
normal-boot path.

## Non-Goals

- No storage I/O, data-region changes, journal changes, or record migration.
- No unified production state loader.
- No production certificate/secret persistence activation or runtime
  configuration mutation; ITEM-324 and ITEM-325 publish policy boundaries
  elsewhere.
- No HTTPS route, authentication, authorization, or management API change.
- No QEMU proof, because this item changes no observable boot behavior.
- No release version change.
