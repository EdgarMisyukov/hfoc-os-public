# HFOC Appliance Configuration Integrity

**Date:** 2026-09-05
**Target:** ITEM-324 of
`docs/superpowers/plans/2026-09-03-hfoc-appliance-platform.md`
**Base SHA:** `8b336fb4dd92e9bba7d24b491c18b794ea5ef441`
**Status:** Completed by ITEM-324

## Purpose

This document is the authoritative ITEM-324 configuration-integrity
boundary. HFOC OS already persists configuration through checksummed atomic
journal records, validates schema at load time, and exposes explicit source
states. ITEM-324 publishes the exact current configuration record families
and a recovery-policy function so future production state loading has one
deny-by-default key and recovery contract instead of scattered literal key
strings and ad-hoc decisions.

The item is a pre-publication policy boundary. It performs no storage I/O,
no boot change, and no modification of existing persistent modules. ITEM-325
completed the certificate/secret persistence policy after this boundary,
ITEM-326 completed the unified appliance health-model boundary, and ITEM-327
completed bounded runtime diagnostics/logs.

## Record Catalogue

The canonical current configuration catalogue is:

| Kind | Name | Namespace | Prefix | Version bounds |
| --- | --- | --- | --- | --- |
| 1 | `system.hostname` | `system.config` | `v1:system.hostname=` | 1..1 |
| 2 | `network.interface` | `system.config` | `v1:network.interface=` | 1..1 |
| 3 | `network.mode` | `system.config` | `v1:network.mode=` | 1..1 |
| 4 | `network.ipv4_address` | `system.config` | `v1:network.ipv4_address=` | 1..1 |
| 5 | `network.ipv4_prefix` | `system.config` | `v1:network.ipv4_prefix=` | 1..1 |
| 6 | `network.ipv4_gateway` | `system.config` | `v1:network.ipv4_gateway=` | 1..1 |
| 7 | `network.ipv4_dns` | `system.config` | `v1:network.ipv4_dns=` | 1..1 |
| 8 | `service.table` | `service.data` | `v1:services=` | 1..1 |
| 9 | `service.entry` | `service.data` | `v1:service.<id>=` | 1..1 |

The `service.entry` ID is a nonzero decimal integer from 1 through 255. Every
record prefix must be followed by at least one value byte. A bounded key may
include an optional single trailing newline.

## Classification

`appliance_config_integrity_classify_record_key()` maps a bounded record key
to an exact record kind, owning namespace, and version. It rejects null and
empty input, unsupported `vN:` versions, unknown record families, bare
prefixes, malformed service entries, and over-limit journal records. Unknown
certificate, secret, diagnostic, or recovery keys remain owned by their later
roadmap items and fail this configuration classifier.

## Recovery Policy

`appliance_config_integrity_recovery_plan()` returns one action from an
existing `config_journal` status and whether the newest record parsed:

- `COMMIT` publishes a valid parsed record.
- `KEEP_LAST_KNOWN_GOOD` preserves an older valid value when the newest
  checksum-valid record is schema-invalid and a known-good value is available.
- `DEFAULT_INVALID` publishes the safe default when no known-good value is
  available.
- `DEFAULT_MISSING` handles an empty journal.
- `EXPLICIT_ERROR` preserves explicit I/O, corruption, bad-device, full, or
  malformed journal-API failure semantics.

The planner itself does not write storage. It is the policy half of an atomic
lower-layer transaction that a future production loader must call before
publication.

## Boundary Contract

`appliance_config_integrity` is allocation-free, read-only, deny-by-default,
and free of storage I/O, diagnostic output, audit writes, blocking, locking,
and interrupt-context access. The immutable catalogue has no initialization
step. Failed lookups never publish an ambiguous kind or namespace.

## Future Ownership

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

This item does not activate those owners or change the current normal-boot
path.

## Non-Goals

- No storage I/O, data-region changes, journal changes, or record migration.
- No unified production state loader.
- No change to existing hostname, network, or service persistence behavior.
- No production certificate/secret persistence activation or runtime
  diagnostics persistence.
- No HTTPS route, authentication, authorization, or management API change.
- No QEMU proof, because this item changes no observable boot behavior.
- No release version change.
