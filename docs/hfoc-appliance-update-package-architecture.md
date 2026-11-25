# HFOC Appliance Update Package Architecture

**Date:** 2026-09-06
**Target:** ITEM-329 of
`docs/superpowers/plans/2026-09-03-hfoc-appliance-platform.md`
**Canonical design:** `docs/superpowers/specs/2026-09-06-item-329-appliance-update-package-architecture-design.md`
**Status:** Completed by ITEM-329

## Purpose

This document is the authoritative ITEM-329 update package architecture
boundary. ITEM-323 through ITEM-328 completed persistent namespaces,
configuration-integrity policy, certificate/secret persistence policy, the
unified health model, bounded runtime diagnostics, and HTTPS health/status.
ITEM-329 fixes the update artifact, bounded metadata, installation stages,
compatibility rules, rollback expectations, failure boundaries, and
`recovery.metadata` ownership before ITEM-330 introduces a manifest parser or
ITEM-331 introduces cryptographic verification.

The architecture preserves the existing deny-by-default and clean-main
boundary. There is no production update transport, listener, storage write,
boot-slot mutation, management API route, or normal-boot activation
introduced by ITEM-329.

## Current State

Reusable lower-layer primitives already exist:

- `kernel/include/update_compatibility.h` and
  `kernel/src/update_compatibility.c` own the bounded 19-field source/target
  compatibility manifest and no-write compatibility gate.
- `kernel/include/candidate_slot_inventory.h` owns the immutable bounded
  candidate-slot inventory.
- `kernel/include/candidate_preflight.h` owns bounded preflight composition.
- `kernel/include/candidate_staged_write.h` owns staged write and readback
  digest verification.
- `kernel/include/activation_intent.h` owns pending target and rollback
  source intent.
- `kernel/include/first_boot_health.h` owns the first-boot health contract.
- `kernel/include/automatic_rollback.h` owns failed-first-boot rollback
  selection.
- `kernel/include/update_acceptance.h` owns healthy acceptance commit.
- `kernel/include/update_retention.h` owns bounded update retention.
- `kernel/include/image_integrity.h`, `kernel/include/recovery_image.h`, and
  `kernel/include/recovery_trust.h` own verified image, recovery-image, and
  recovery-trust gates.
- `kernel/include/release_profile.h`, `release/release-upgrade-matrix.txt`,
  and `scripts/make-release-update-bundle.sh` publish release identity,
  allowed upgrade paths, and a deterministic release update-bundle fixture.

ITEM-323 reserved the `recovery.metadata` persistent namespace with canonical
`v1:recovery.` prefixes and PRIVATE sensitivity. Existing Wave P QEMU proof
and release fixtures do not constitute a production update path.

There is no production update transport, package admission route, storage
writer, boot-slot switcher, recovery-mode service, or factory-reset
operation. This document only records the architecture that later focused
items must implement.

## Scope

This item changes documentation and a targeted host fixture only:

- this authoritative update package architecture document;
- the focused design and implementation plan in
  `docs/superpowers/specs/2026-09-06-item-329-appliance-update-package-architecture-design.md`
  and
  `docs/superpowers/plans/2026-09-06-item-329-appliance-update-package-architecture-implementation.md`;
- ITEM-329/ITEM-330 status and metadata updates;
- one targeted host test that guards the architecture and roadmap contract.

No kernel, QEMU, protocol, storage, interrupt, network, management API,
release-version, or normal-boot behavior changes are in scope.

## Architecture Decisions

1. An update is an immutable candidate package that is never treated as a
   general archive, executable, script, writable filesystem, or shell input.
2. The package is bounded, versioned, and deny-by-default. Unknown fields,
   unknown components, malformed ranges, unsupported versions, and duplicate
   identity values are rejected before mutation.
3. Validation order is deterministic: bounded identity/compatibility parse
   and validation, then integrity/trust verification, then candidate-slot
   preflight, then staged write, then activation, reboot, health acceptance,
   or automatic rollback.
4. Existing Wave P boundaries are the implementation seam. ITEM-329 does not
   create parallel update abstractions or redefine already published release
   compatibility contracts.
5. Every persistent update record belongs under the ITEM-323
   `recovery.metadata` namespace with the canonical `v1:recovery.` prefix.
6. No update package may mutate active state before all required metadata,
   compatibility, and integrity/trust checks succeed.
7. Recovery mode and factory reset remain separate later boundaries. ITEM-332
   owns recovery mode and ITEM-333 owns factory reset.

## Artifact Contract

The candidate update package is one bounded immutable artifact. It carries
logical layers that later focused items must parse and verify:

| Layer | Purpose | Existing contract |
| --- | --- | --- |
| Identity | exact release/profile and image identity | `release_profile` |
| Compatibility | exact source/target ranges | `update_compatibility` |
| Integrity | canonical SHA-256 digest(s) of package/payload | `image_integrity`, SHA-256 |
| Trust | update-purpose signed metadata and signer binding | `signed_metadata`, `image_policy` |
| Payload | bounded named system-image components | `system_image` |

The exact byte framing, parser rules, and serialized metadata catalogue are
owned by the ITEM-330 focused design. ITEM-329 fixes the architecture that
ITEM-330 must not violate: no unbounded input, no arbitrary path/name/script
content, no implicit source identity, no secret payload, and no candidate
without an exact digest.

The current `scripts/make-release-update-bundle.sh` output is a deterministic
release-manifest fixture. ITEM-329 does not repurpose that script as a
runtime installer and does not require it to carry payload bytes.

## Metadata Contract

The architecture requires bounded metadata that can populate or be checked
against these exact logical values:

- `update_package_version`;
- `source_revision`;
- target release identity from `release_profile`, including `HFOC-APPLIANCE-V1`
  and `x86_64`;
- source and target image IDs;
- source/target schema, kernel ABI, service ABI, and policy-generation
  inclusive ranges;
- canonical SHA-256 digest(s) for package metadata and payload(s);
- a signed-metadata purpose of `update` and the signer/policy generation
  required for verification.

The metadata catalogue must reject duplicate, missing, unknown, empty,
over-limit, and semantically inconsistent values. Raw credentials, private
keys, tokens, secrets, shell commands, and file paths never appear in update
metadata or audit/diagnostic output.

## Installation Stages

The architecture defines the following stage model:

1. Package admission through an approved future transport/management
   boundary. ITEM-329 adds no transport or listener.
2. Bounded identity/compatibility metadata parse and validation. ITEM-330
   implements the manifest parser/validator.
3. Integrity and signature verification. ITEM-331 implements cryptographic
   integrity/authenticity checks and fail-closed behavior.
4. Candidate-slot inventory lookup and preflight using existing
   `candidate_slot_inventory` and `candidate_preflight`.
5. Staged write with readback digest verification using existing
   `candidate_staged_write`.
6. Activation intent with the current known-good entry as rollback source
   using existing `activation_intent`.
7. Reboot into the pending target and first-boot health observation using
   existing `first_boot_health`.
8. Acceptance commit only after a complete healthy first-boot contract, or
   automatic rollback to the pending rollback source after a failed
   first-boot health contract.
9. Bounded retention/reclamation of unreferenced inactive candidates using
   existing `update_retention`.

## Compatibility Rules

The architecture fixes these compatibility rules:

- the target package must match the active release profile constants;
- the source image ID must equal the active/known-good image ID;
- the target image ID must be distinct, bounded, and present in the active
  candidate inventory;
- every source/target range must be inclusive, internally consistent, and
  accepted by the existing `update_compatibility_check()` contract;
- the canonical release upgrade matrix remains authoritative for allowed
  forward and pre-acceptance rollback paths;
- same-image, unknown-image, unsupported-architecture, unsupported-version,
  and out-of-range candidates are rejected before any inventory or intent
  mutation.

Compatibility failures are ordinary recoverable errors, not kernel panics.

## Rollback Expectations

The architecture defines:

- a pending activation always records the current known-good entry as the
  rollback source;
- automatic rollback is allowed only before acceptance and only for a failed
  first-boot health contract;
- a torn activation-intent persistence record resolves to the previous
  known-good entry, never to the pending candidate;
- after healthy acceptance, the accepted candidate becomes the known-good
  entry and no post-acceptance automatic rollback is introduced by this item;
- managed recovery and factory reset remain owned by ITEM-332 and ITEM-333;
- retention protects active, rollback, and pending references and reclaims
  only bounded unreferenced inactive slots.

## Failure Boundaries

The architecture defines no-mutation failure boundaries:

- parse, compatibility, integrity, and trust failures occur before candidate
  inventory publication, staged write, activation intent, or known-good
  changes;
- staged-write and readback failures never mark a candidate ready;
- activation-intent persistence failure leaves no published pending target;
- incomplete or failed first-boot health prevents acceptance and selects the
  pending rollback source when the existing automatic-rollback contract
  applies;
- retention never reclaims active, rollback, or pending references;
- no update failure creates a general writable filesystem, new interrupt
  path, storage driver, boot-slot mutation, or management activation.

## Recovery Metadata Ownership

Update lifecycle state belongs to the `recovery.metadata` namespace from
ITEM-323. Future production record kinds must use the `v1:recovery.` prefix
and exact bounded record families derived from the existing Wave P
boundaries:

- `update_compatibility`;
- `candidate_slot_inventory`;
- `candidate_preflight`;
- `candidate_staged_write`;
- `activation_intent`;
- `first_boot_health`;
- `automatic_rollback`;
- `update_acceptance`;
- `update_retention`;
- `image_integrity`;
- `recovery_image`;
- `recovery_trust`;
- `trust_anchor`;
- `trust_revocation`;
- `trust_rotation`.

No raw update payload, private key, credential, session token, or secret is
stored under this namespace.

## Wave H Boundaries

Wave H ownership is assigned as follows:

- ITEM-329: update package architecture documentation and its host-guarded
  contract;
- ITEM-330: manifest validation and compatibility checks;
- ITEM-331: cryptographic integrity and signature verification;
- ITEM-332: recovery mode;
- ITEM-333: factory reset.

Controlled mutating update management remains deferred until ITEM-330/331 and
the corresponding authenticated management boundary are approved and proven.

## Non-Goals

- No manifest parser or package validation implementation; ITEM-330 owns it.
- No cryptographic integrity or signature verification implementation;
  ITEM-331 owns it.
- No recovery mode or factory reset implementation; ITEM-332 and ITEM-333 own
  them.
- No production normal-boot update listener, transport, storage write,
  boot-slot switch, management route, or controlled operation.
- No new QEMU proof, because this item changes no runtime behavior.
- No release, version, or tag change.

## Acceptance

The focused host fixture `tests/test-item-329-update-package-architecture.sh`
guards this authoritative document, the roadmap status, and the metadata
links. The document records actual current state without claiming a
production update path, fixes the artifact/metadata/stage/compatibility/
rollback/failure/recovery ownership decisions above, and leaves ITEM-330 as
the next incomplete item in the roadmap.

## Files

- `docs/superpowers/specs/2026-09-06-item-329-appliance-update-package-architecture-design.md`
- `docs/superpowers/plans/2026-09-06-item-329-appliance-update-package-architecture-implementation.md`
- `docs/hfoc-appliance-update-package-architecture.md`
- `tests/test-item-329-update-package-architecture.sh`
- This authoritative document
