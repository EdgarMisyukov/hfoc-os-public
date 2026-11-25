# HFOC Production End-to-End Acceptance

**Date:** 2026-09-11
**Target:** ITEM-369 of the HFOC Appliance Platform production wave
**Release contract:** HFOC-APPLIANCE-V1
**Status:** Implementation and focused proof complete on the feature branch;
branch CI, Ready, merge, and post-merge gates are pending.

## Purpose

This document records the ITEM-369 production end-to-end acceptance gate for
the production integration wave (ITEM-360 .. ITEM-368). Before this item the
individual production proofs existed but no single gate turned them into a
per-stage verdict: several QEMU proofs ran only inside the QEMU suite, and the
auth, management, mutation, update and recovery-executor planes had host-only
proofs. ITEM-369 owns the canonical stage matrix, the strict verifier, and the
machine-readable report that walks the production path
`media -> installer -> storage/state -> first boot -> network -> DHCP/DNS ->
HTTPS/service -> auth -> management -> mutations -> update -> reboot ->
recovery` and records every stage as `PASS`, `DEGRADED`, `NOT_TESTED` or
`FAIL`.

Physical acceptance is not part of this gate. Per AGENTS.md section 3a and the
appliance productization queue, a physical run belongs only to the
hardware-acceptance item (ITEM-381), is never improvised in another item, and
never runs in CI. The matrix therefore declares
`stage_24_id=HARDWARE_ACCEPTANCE` as `NOT_TESTED` owned by `ITEM-381`, the
verifier refuses any attempt to turn it into a proven stage, and
`release/hfoc-hardware-acceptance-report.md` must keep
`**Status:** NOT TESTED`.

## Canonical Matrix

`release/hfoc-production-end-to-end-acceptance.txt` (schema version 2)
declares 25 ordered stages. A `PROVEN` or `DEGRADED` stage names exactly one
Makefile target and the exact marker that target prints; a `DEGRADED` stage
also carries a machine-readable `reason`, so the matrix itself states why the
stage is not a full pass; a `NOT_TESTED` stage names its owner item and the
reason it cannot be tested in this wave. Only `HARDWARE_ACCEPTANCE` (ITEM-381)
may stay `NOT_TESTED`: every stage owned by an
implemented item (ITEM-366, 367A, 367B, 368 and 371..378) must be `PROVEN` or
`DEGRADED`, and the verifier refuses a matrix that silently skips one
(`required_executable_stages`). ITEM-382 appends its own
`LICENSE_PAYLOAD` stage last, so the stage numbers a release decision already
reads for `SBOM_RELEASE` and `HARDWARE_ACCEPTANCE` do not move.

| Stage | Owner | State | Target | Marker |
| --- | --- | --- | --- | --- |
| `MEDIA_DIGEST` | ITEM-347 | PROVEN | `test-release-artifact-host` | `Release artifact manifest verification: OK` |
| `INSTALLER` | ITEM-361 | PROVEN | `test-production-installer-qemu` | `QEMU production-installer test: OK` |
| `STORAGE_STATE` | ITEM-360 | PROVEN | `test-production-storage-boot-qemu` | `Production storage boot QEMU test: OK` |
| `FIRST_BOOT` | ITEM-363 | PROVEN | `test-production-first-boot-qemu` | `QEMU production first-boot test: OK` |
| `NETWORK_LINK` | ITEM-362A | PROVEN | `test-production-network-qemu` | `QEMU production network test: OK` |
| `DHCP_DNS` | ITEM-362B | PROVEN | `test-production-dhcp-dns-qemu` | `QEMU production DHCP/DNS test: OK` |
| `HTTPS_SERVICE` | ITEM-364 | DEGRADED | `test-production-service-https-qemu` | `QEMU production service activation test: OK` |
| `AUTH` | ITEM-365 | PROVEN | `test-appliance-production-auth-host` | `Appliance production auth host tests: ALL OK` |
| `MANAGEMENT` | ITEM-366 | PROVEN | `test-appliance-production-management-plane-host` | `Appliance production management plane host tests: ALL OK` |
| `MUTATION` | ITEM-367A | PROVEN | `test-appliance-production-mutation-plane-host` | `Appliance production mutation plane host tests: ALL OK` |
| `UPDATE` | ITEM-367B | PROVEN | `test-appliance-production-update-plane-host` | `Appliance production update plane host tests: ALL OK` |
| `REBOOT_PERSISTENCE` | ITEM-335 | PROVEN | `test-persistent-config-reboot-acceptance-qemu` | `Persistent configuration reboot acceptance: OK` |
| `RECOVERY` | ITEM-368 | PROVEN | `test-production-recovery-reset-qemu` | `Production recovery reset QEMU test: OK` |
| `RECOVERY_EXECUTOR` | ITEM-368 | PROVEN | `test-appliance-production-recovery-executor-host` | `Appliance production recovery executor host tests: ALL OK` |
| `SIGNED_MEDIA` | ITEM-371 | DEGRADED | `test-installer-hardening-host` | `Installer media and hardening host tests: ALL OK` |
| `SIGNED_BOOT` | ITEM-372 | PROVEN | `test-production-boot-chain-qemu` | `QEMU production boot chain test: OK` |
| `STORAGE_ENCRYPTION` | ITEM-373 | DEGRADED | `test-state-encryption-host` | `Appliance state encryption host tests: ALL OK` |
| `TIME_SYNC` | ITEM-374 | DEGRADED | `test-appliance-network-mode-host` | `Appliance network mode host tests: ALL OK` |
| `TLS_LIFECYCLE` | ITEM-375 | DEGRADED | `test-appliance-management-tls-host` | `Appliance management TLS host tests: ALL OK` |
| `UPDATE_ROLLBACK` | ITEM-376 | DEGRADED | `test-appliance-update-lifecycle-host` | `Appliance update lifecycle host tests: ALL OK` |
| `AUDIT_EXPORT` | ITEM-377 | DEGRADED | `test-appliance-audit-chain-host` | `Appliance audit chain host tests: ALL OK` |
| `SUPPORT_BUNDLE` | ITEM-378 | DEGRADED | `test-appliance-production-support-host` | `Appliance production support host tests: ALL OK` |
| `SBOM_RELEASE` | ITEM-380 | PROVEN | `test-product-release-gate-host` | `Release gate host test: OK` |
| `HARDWARE_ACCEPTANCE` | ITEM-381 | NOT_TESTED | — | — |
| `LICENSE_PAYLOAD` | ITEM-382 | PROVEN | `test-license-payload-host` | `License payload host test: OK` |

`HTTPS_SERVICE` is deliberately `DEGRADED`, with the remaining gap measured:
the ordinary boot raises a live TLS 1.3 listener with a hardware entropy owner
and a per-appliance certificate, and ITEM-386 fixes the production NIC path
below it -- receive replenishment and transmit recycling -- so the listener now
receives the client's complete first flight (`rx_bytes` 536 -> 1483 -> 2006,
measured on the production image) instead of one segment. The request/response
proof over TLS still does not complete: writing the TLS 1.3 ServerHello returns
`MBEDTLS_ERR_SSL_HANDSHAKE_FAILURE` (-0x6E00, no alert pending) after a
successful HelloRetryRequest, with a valid certificate pair and a healthy
connection arena, so the receive path is no longer the blocker.

`TIME_SYNC` is deliberately `DEGRADED`: the focused host proof covers the
static and offline mode decisions, the honest offline `DEGRADED` verdict and
the fail-closed management ingress binding, and the `DHCP_DNS` QEMU proof now
also leases DHCP, resolves the gateway by ARP and synchronizes the wall clock
over SNTP (`clock=READY`, `QEMU production network/time test: OK`) before TLS
can start. It stays `DEGRADED` because there is no static-mode or no-NIC QEMU
run yet, no RTC exists in the frozen baseline, and the management listener
that must consume the binding belongs to ITEM-375.

`STORAGE_ENCRYPTION` is deliberately `DEGRADED`: the focused proof seals the
state volume at install under a passphrase-derived key with an escrowed
recovery key, refuses a wrong key and refuses tampered ciphertext, but the
frozen baseline has no TPM backend and no unattended ordinary-boot key
provider, so an unattended boot of a sealed volume publishes `STATE_LOCKED`
with `DEGRADED` readiness instead of a fabricated unlock. The stage therefore
never renders as a full pass until a TPM or console key provider is bound.

`UPDATE_ROLLBACK` is deliberately `DEGRADED`: the focused proofs publish the
durable two-copy update record with its torn-write fallback, verify a real
ECDSA P-256 signed bundle under the declared update purpose, refuse a
downgrade and a foreign source image, and resolve the boot decision
(handover, attempt budget, damaged-candidate rollback, accept, rollback), and
the extended `test-production-storage-boot-qemu` proves on a real disk that
ordinary boot writes exactly the declared record blocks and reloads the
record on the next boot. It stays `DEGRADED` because the frozen baseline has
no production candidate payload region, so the slot readback is fail-closed
and a pending record always rolls back; physical power loss is `NOT TESTED`
and a bootloader slot switch is out of scope.

`AUDIT_EXPORT` is deliberately `DEGRADED`: `appliance_audit_chain` owns the
bounded SHA-256 hash chain over a canonical record encoding with tamper
detection at the exact sequence and rotation that moves the anchor so the
retained window still verifies, `appliance_audit_export` owns the cursor-based
external export (absolute or oldest-retained start, a declared gap when the
retention window moved past the consumer, contiguous acknowledgement), and the
production management snapshot renders the integrity view
(`audit_chain=OK|TAMPERED|UNAVAILABLE audit_records=N audit_head=...`). Host
proofs `test-appliance-audit-chain-host` (71 checks),
`test-appliance-audit-export-host` (44 checks) and
`test-appliance-production-audit-host` (38 checks); the extended
`test-production-storage-boot-qemu` requires the verified
`[AUDIT-INTEGRITY]` chain on ordinary boot and the extended
`test-service-supervisor-qemu` requires the panel view. It stays `DEGRADED`
because the frozen baseline has no persistent audit store: the chain and its
cursor are volatile across a reboot, retention drops the oldest half once the
window is full, forwarding the export to a remote collector is not implemented,
and physical tamper of stored bytes is `NOT TESTED`.

`SBOM_RELEASE` is `PROVEN`: `scripts/generate-release-sbom.sh` writes a
deterministic SPDX 2.3 SBOM from `third_party/LICENSES.md` and
`third_party/LOCKS.sha256` (product and dependency packages, the mbedTLS
`Apache-2.0` election, one file entry per locked vendored file with its exact
SHA-256, and the `DESCRIBES`/`DEPENDS_ON`/`CONTAINS` relationships),
`scripts/verify-release-sbom.sh` re-derives and refuses every mismatch, the ISO
build is pinned through `SOURCE_DATE_EPOCH` and `ISO_REPRODUCIBLE_FLAGS` so
`test-reproducible-image-host` compares the ISO digest itself,
`docs/hfoc-security-response-runbook.md` with
`scripts/verify-cve-runbook.sh` defines and enforces the vulnerability response
process, and `scripts/verify-product-release-gate.sh` accepts a release only
when the SBOM, the reproducible ISO evidence and the runbook are all present
and valid for the current source revision. Host proof
`test-product-release-gate-host`; `make test-product-release-gate` is the
release-time composition and is deliberately not part of `make test`.
`LICENSE_PAYLOAD` is `PROVEN` and owned by ITEM-382: the license payload that
ships on every image is installed from the declared dependency inventory,
`scripts/verify-license-payload.sh` verifies the exact file set, the manifest
digest of every file and the canonical AGPL text, and
`scripts/verify-license-notices.sh` forces every declared dependency notice —
including the shipped mbedTLS `Apache-2.0` election notice — into the payload
and into its `LICENSES.sha256`. The stage fails closed when the payload is
missing, incomplete, carries an undeclared notice or has a manifest that does
not match. `test-license-payload-host` proves the positive payload and the
negative fixtures (a missing notice, a renamed election notice, an uncovered
notice, an undeclared notice and a dependency declared without a shipped
notice), each failure naming the dependency.

`SUPPORT_BUNDLE` is deliberately `DEGRADED`: `appliance_support_bundle` owns
the canonical `HFOC-SB-BIND-1` binding over the exported ITEM-168 bytes, the
redaction attestation that re-applies the ITEM-169 deny list and refuses a
bundle with a leaked secret, and the keyed HMAC-SHA-256 authenticity seal with
`UNSIGNED`/`MAC_UNAVAILABLE` published honestly instead of a fabricated
signature; `appliance_support_session` keeps a remote session inert until a
locally present administrator consents, closes the scope vocabulary to
read/export with no shell bit, bounds and revokes the grant and audits every
transition; `appliance_support_break_glass` is the documented shell-free way
back in with a fixed action set, local-presence-only arming and a two-phase
single-use token; and `appliance_production_support` performs the real effects
(restore `LOCAL_ONLY` ingress, revoke the live session) and feeds the ITEM-366
management snapshot. Proofs `test-appliance-support-bundle-host`,
`test-appliance-support-session-host`,
`test-appliance-support-break-glass-host`,
`test-appliance-production-support-host` and the extended
`test-appliance-production-management-host`. It stays `DEGRADED` because the
frozen baseline has no on-device asymmetric signing identity — the seal is a
keyed HMAC-SHA-256 authenticity tag, and the verification-only Mbed TLS profile
has no RNG or DRBG to sign with — because ordinary boot has no bundle source
store yet, and because the session audit window is volatile across a reboot.

`TLS_LIFECYCLE` is deliberately `DEGRADED`: the focused host proof publishes
the frozen TLS 1.3 policy, refuses every legacy protocol, weak cipher, static
key exchange, weak signature, compression and renegotiation, proves the
certificate request/import contract and rotation without downtime against the
fail-closed management ingress binding, and the extended
The `HTTPS_SERVICE` stage runs the public `test-production-service-https-qemu`
target, which composes two independently scheduled parts:
`test-production-service-https-main-qemu` (the main HTTPS proof) and
`test-production-service-https-noentropy-qemu` (the no-RNG/operator-seed
proof).  In CI both parts run once through the QEMU suite's external queue;
the acceptance gate requires the current run's harvest to name both as PASS,
and the aggregate prints its canonical marker only after both succeeded.

The `test-production-service-https-qemu` proof requires the honest
`[PRODUCTION-TLS]` record on ordinary boot. ITEM-404 makes the lifecycle real
on a boot whose clock is validated: the serving path consumes the live SNTP
epoch, imports the certificate the listener actually serves (with its parsed
validity window and DER fingerprint) and reaches `ACTIVE`; the extended
`test-production-dhcp-dns-qemu` proof requires `state=ACTIVE
certificate=READY import=1 reject=0` after `[PRODUCTION-NET-MODE] mode=DHCP
clock=READY`. It stays `DEGRADED` because the frozen baseline has no RTC — a
boot without a validated clock publishes `AWAITING_CERTIFICATE` while the
listener serves the same certificate — and because operator-driven rotation is
not exercised on ordinary boot (the ITEM-325 request/import/promote path and
its staged rotation are host-proven, and physical rotation is `NOT_TESTED`).

## Gate Behavior

`scripts/verify-production-end-to-end-acceptance.sh` strictly validates the
matrix: version, release identity, stage count, declared-stage numbering,
field counts, unique IDs and names, the fixed stage order, state and kind
domains, target/marker syntax, Makefile target existence, marker presence in a
real proof artifact, doc coverage, `NOT_TESTED` stages that publish no target,
and the hardware-acceptance guard.

`scripts/test-production-end-to-end-acceptance.sh` runs the gate. Evidence
harvesting is opt-in through `HFOC_PRODUCTION_ACCEPTANCE_HARVEST_DIR`: when it
names a directory, `PASS` entries in a suite `suite-status.txt` or an
acceptance `manifest.txt` under it satisfy the matching stage instead of
re-running its target. CI points it at the QEMU suite directory of the same
job after that directory has been reset, so no stale run can satisfy a stage.
Every remaining `run`/`gate` stage executes independently through
`scripts/run-acceptance-suite.sh` (one stage per invocation): a stage that
fails cannot stop the stages after it from producing their own verdict, so the
report always covers the whole path. The runner then writes `report.txt` into
`build/test-artifacts/production-end-to-end-acceptance-<UTC>-<pid>/` (CI pins
`HFOC_PRODUCTION_ACCEPTANCE_ARTIFACT_DIR` to a stable path and uploads it), and
ends with `PRODUCTION END-TO-END ACCEPTANCE: ACCEPT` only when every `PROVEN`
stage printed its marker `PASS` and every `DEGRADED` stage printed its marker
`DEGRADED`; any failing or unobserved stage yields `gate_status=REJECT`, the
`gate_reason` names the **first** such stage, and the runner prints
`PRODUCTION END-TO-END ACCEPTANCE: REJECT`.

### Report contract

`report.txt` is the machine-readable artifact a release decision reads
(`hfoc_production_end_to_end_acceptance_report_version=2`). It carries the
release identity, the source revision, the stage count, the artifact directory,
the UTC run window (`run_started_utc`, `run_finished_utc`) and one ten-field
block per stage: `id`, `name`, `owner`, `expected`, `status`, `evidence`,
`target`, `marker`, `detail` and `reason`.

`status` is the verdict the matrix declares: `PASS` for an observed `PROVEN`
stage, `DEGRADED` for an observed `DEGRADED` stage (never `PASS`), and
`NOT_TESTED` for a stage this wave cannot test, with its reason. A stage whose
target ran but did not print its declared marker is `FAIL`; a stage with no
evidence at all is `UNOBSERVED`. `gate_status=ACCEPT` forbids `FAIL` and
`UNOBSERVED`; `gate_status=REJECT` requires at least one of them.

`scripts/verify-production-end-to-end-acceptance-report.sh` validates the
report against that contract, and with an optional matrix argument it also
requires every stage identity, owner, expected state, target and marker to
match the canonical matrix. The runner validates its own report with it before
it can print `ACCEPT`, so a malformed report is a failure rather than an
artifact.

## Software Proof

| Gate | Target | Evidence |
| --- | --- | --- |
| Acceptance host fixture | `make test-production-end-to-end-acceptance-host` | `Production end-to-end acceptance host test: OK` |
| Real end-to-end gate | `make test-production-end-to-end-acceptance` | `PRODUCTION END-TO-END ACCEPTANCE: ACCEPT` |
| Full regression | `make test` | PASS (branch CI) |

The host fixture proves the canonical matrix verifies, that every malformed or
fabricated variant is rejected (version, release identity, stage count,
undeclared stage, duplicate ID/name, unknown state, missing kind, malformed
target/marker, missing Makefile target, missing proof marker, `NOT_TESTED`
stage with a target, a `DEGRADED` stage without its machine-readable reason, an
implemented stage silently skipped to `NOT_TESTED`, reordered stages, and a
hardware stage flipped to `PROVEN` while the hardware report still says
`NOT TESTED`), that the runner accepts a passing stage set, harvests existing
evidence instead of re-running a target, reports `DEGRADED` for a degraded
stage, keeps producing verdicts for the stages after a failing one, names the
first failure in `gate_reason`, and that the report verifier accepts a
well-formed report and rejects a tampered one (a `DEGRADED` stage rendered as
`PASS`, a rewritten status under `ACCEPT`, a missing `gate_reason`, a missing
stage verdict). The fixture never starts QEMU.

## Limitations

- Physical UEFI Secure Boot, firmware-enforced signature checking, physical
  media and physical hardware acceptance remain `NOT_TESTED`; the signed boot
  chain publishes an honest `ATTESTED`/`UNATTESTED`/`UNAVAILABLE` verdict and
  never claims a firmware root of trust. Release-key media signing is owned by
  ITEM-380 and physical acceptance by ITEM-381.
- `SIGNED_MEDIA` is deliberately `DEGRADED`: the focused proof passes and the
  production ECDSA P-256 verifier is bound (ITEM-372), but no release-signed
  media exist yet, so a deployed installer still fails closed at the signature
  boundary instead of shipping a verified install. The media envelope, the signed media
  and source digests, the fail-closed supported-hardware policy, the
  plan/preview/confirm gate and the uninstall/return-to-previous-OS contract are
  proven by `make test-installer-hardening-host`, and the QEMU
  production-installer proof additionally refuses to write before the media and
  hardware gates pass (with zero disk writes). A `DEGRADED` stage passes its own
  proof but is never rendered as a full pass.
- `HTTPS_SERVICE` is `DEGRADED`: ITEM-386 supplies the ordinary-boot entropy and
  certificate owners, raises the TLS endpoint and fixes the production NIC ring
  cycle (receive replenishment and transmit recycling) that used to stall a
  client's first flight; the remaining failure is the serving profile's TLS 1.3
  ServerHello write, measured as `MBEDTLS_ERR_SSL_HANDSHAKE_FAILURE` (-0x6E00).
- `TIME_SYNC` is `DEGRADED`: SNTP synchronization and the TLS clock gate are
  proven, but offline and static runtime fixtures and the management listener
  binding are not in this wave, so a clock-less or network-less boot honestly
  stays `UNSYNCHRONIZED`/`DEGRADED` instead of fabricating a clock.
- `STORAGE_ENCRYPTION` is `DEGRADED`: `make test-state-encryption-host` proves
  the sealed install, the passphrase and recovery-key custody, the escrow line,
  the fail-closed wrong-key path and tamper detection, but the unattended TPM
  binding is `UNAVAILABLE` in the frozen baseline and ordinary boot has no
  key provider, so a sealed volume publishes `STATE_LOCKED` with `DEGRADED`
  readiness. Physical TPM sealing remains `NOT_TESTED`.
- The host-only planes (auth, management, mutation, update, recovery executor)
  are accepted at their documented composition boundary; binding them to an
  ordinary-boot request path is later scope.
- No VERSION or release-number change.

## Non-Goals

- No new kernel, boot, storage, network, service, management, recovery, or
  release behavior.
- No physical acceptance and no `/dev/sda` or `/dev/nvme0n1` access.
- No VERSION change and no v1.0.0 tag.
