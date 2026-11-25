# HFOC Appliance Factory Reset

**Date:** 2026-09-06
**Target:** ITEM-333 of
`docs/superpowers/plans/2026-09-03-hfoc-appliance-platform.md`
**Base SHA:** `7ceeb744b5e3f481c5fc3bd3c203cdacc81df5e7`
**Status:** Completed by ITEM-333

## Purpose

This document is the authoritative ITEM-333 appliance factory-reset boundary.
ITEM-333 defines a controlled factory-reset operation policy before HFOC OS
has a production persistent-state loader. The boundary publishes the exact
ITEM-323 namespace state map and a one-shot gate that a future ITEM-334/335
production storage loader must execute.

The item is deliberately a pre-production policy and gate boundary. It
performs no storage I/O, no boot-slot or image mutation, no management route
change, and no normal-boot activation. ITEM-334/335 own production boot
storage discovery and reboot acceptance.

## Factory Reset Model

`appliance_factory_reset` consumes caller-owned evidence:

| Evidence field | Meaning |
| --- | --- |
| `authorization_verified` | approved administrative authorization for reset |
| `boot_foundation_verified` | verified normal/recovery boot foundation for safe restart |
| `storage_loader_ready` | production persistent-state loader/device path is ready |
| `recovery_trust_active` | active recovery trust denies mutation |

The module does not verify authentication, authorization, boot evidence, or
storage readiness. It publishes no reset plan unless every caller-owned
prerequisite is true and recovery trust is inactive.

## State Map

The exact namespace plan is:

| Namespace ID | Action |
| --- | --- |
| `SYSTEM_CONFIG` | `REWRITE_FACTORY_DEFAULT` |
| `SERVICE_DATA` | `CLEAR` |
| `CERTIFICATE` | `CLEAR` |
| `SECRET` | `CLEAR` |
| `DIAGNOSTIC` | `CLEAR` |
| `RECOVERY_METADATA` | `PRESERVE_FOUNDATION` |

`SYSTEM_CONFIG` is rewritten to validated safe defaults rather than removed.
Service data, certificates, secrets, and diagnostics are cleared. Recovery
metadata is preserved as the minimum recovery foundation. Boot code, Limine,
kernel, and normal/recovery system-image payloads are outside the namespace
plan and are never reset by this boundary.

## API

`kernel/include/appliance_factory_reset.h` publishes:

- `APPLIANCE_FACTORY_RESET_VERSION`;
- exact status and action constants and strings;
- `struct appliance_factory_reset_evidence`;
- `struct appliance_factory_reset_namespace_plan`;
- `struct appliance_factory_reset_plan`;
- `struct appliance_factory_reset_gate`;
- `appliance_factory_reset_init()`;
- `appliance_factory_reset_evaluate()`;
- `appliance_factory_reset_is_published()`;
- status/action string helpers.

`appliance_factory_reset_init()` publishes an empty gate.
`appliance_factory_reset_evaluate()` validates evidence in no-write order,
rejects active recovery trust, missing authorization, missing boot
foundation, and unready storage, then publishes the exact six-namespace plan
and gate atomically.

## Gate Semantics

The gate is a one-shot reset-decision record. It owns no credential, storage,
boot image, service, or normal-boot object. Reinitialization and reuse of a
published gate are rejected. Every refusal leaves the caller gate and plan
byte-for-byte unchanged.

## Validation And Failure

- Null gate, evidence, or plan output is rejected.
- Unknown evidence version is rejected.
- An uninitialized or already published one-shot state is rejected.
- Active recovery trust denies reset with
  `APPLIANCE_FACTORY_RESET_RECOVERY_TRUST_ACTIVE`.
- Missing authorization denies with `UNAUTHORIZED`.
- Missing boot/recovery foundation denies with
  `BOOT_FOUNDATION_UNAVAILABLE`.
- Unready persistent loader denies with `STORAGE_NOT_READY`.
- The module performs no allocation, blocking, storage I/O, audit write,
  diagnostic output, locking, or interrupt-context access.
- No raw credentials, private keys, tokens, secrets, shell commands, file
  paths, or record values ever appear in reset evidence, plans, or this
  module.
- No release, tag, VERSION, management route, or normal-boot activation is
  introduced.

## Future Ownership

- ITEM-334: integrated appliance boot acceptance.
- ITEM-335: persistent configuration reboot acceptance.
- A future focused item may define exact `recovery.metadata` foundation
  record families before a production loader executes this plan.

This item does not activate those owners or change the current normal-boot
path.

## Non-Goals

- No actual persistent storage write, erasure, journal compaction, or block
  device mutation.
- No boot-slot switch, system-image/recovery-image rewrite, or normal-boot
  activation.
- No local/HTTPS route, credential reset, or management authorization
  change.
- No QEMU proof, because this item changes no observable boot behavior.
- No release version change.
