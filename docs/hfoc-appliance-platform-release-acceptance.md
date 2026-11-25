# HFOC Appliance Platform Release Acceptance

**Date:** 2026-09-06
**Target:** ITEM-337 of
`docs/superpowers/plans/2026-09-03-hfoc-appliance-platform.md`
**Base SHA:** `6c2916344e7dc625a1ebdd7fdddd60e5fe091d95`
**Status:** Completed by ITEM-337

## Purpose

This document is the authoritative HFOC Appliance Platform release
acceptance boundary. ITEM-337 records the final proof inventory that must be
present before the appliance platform roadmap can be called complete. It
does not add production behavior or duplicate QEMU execution. The real
proofs run through `make test`; the ITEM-337 gate verifies the canonical
matrix against `Makefile`, real proof artifacts, and this document after the
full regression suite.

## Release Matrix

`release/hfoc-appliance-platform-release-acceptance.txt` is the canonical
final matrix. It declares the exact release identity, twelve required
categories, and the target plus log marker that owns each category.

| Category | Target | Marker |
| --- | --- | --- |
| `BOOT` | `test-integrated-appliance-boot-acceptance-qemu` | `Integrated appliance boot acceptance: OK` |
| `PERSISTENCE` | `test-persistent-config-reboot-acceptance-qemu` | `Persistent configuration reboot acceptance: OK` |
| `MANAGEMENT` | `test-management-plane-qemu` | `QEMU management plane acceptance test: OK` |
| `INSTALLER` | `test-hfoc-integrated-installer-dual-boot-management-acceptance` | `HFOC integrated installer/dual-boot/management acceptance gate: OK` |
| `NETWORK` | `test-appliance-network-config-qemu` | `QEMU network config test: OK` |
| `SERVICES` | `test-service-supervisor-qemu` | `QEMU supervisor composition test: OK` |
| `HTTPS` | `test-https-vertical-slice-acceptance` | `HTTPS vertical-slice acceptance gate: ACCEPT` |
| `SECURITY` | `test-release-security-baseline` | `Release security baseline: OK` |
| `RECOVERY` | `test-release-disaster-recovery-rehearsal` | `Appliance v1 disaster-recovery rehearsal: OK` |
| `UPDATE` | `test-update-lifecycle-qemu` | `QEMU update-lifecycle test: OK` |
| `RESET` | `test-appliance-factory-reset-host` | `appliance factory reset host: OK` |
| `RELEASE` | `test-release-appliance-v1-acceptance` | `Appliance v1 acceptance gate: ACCEPT` |

## Gate

`scripts/verify-hfoc-appliance-platform-release-acceptance.sh` validates the
matrix shape, category order, uniqueness, Makefile targets, real proof
markers, and this document. `scripts/test-hfoc-appliance-platform-release-acceptance.sh`
runs the verifier and prints the final gate verdict.

`make test-hfoc-appliance-platform-release-acceptance-host` runs the fixture
test and `make test-hfoc-appliance-platform-release-acceptance` runs the real
gate. `make test` invokes the real gate after the post-acceptance QEMU
regression, and `make check` verifies the matrix and host fixture.

## Completion

ITEM-337 completes the HFOC Appliance Platform roadmap when:

- ITEM-334 through ITEM-336 are merged and their QEMU proofs are green;
- the existing Appliance v1 and HTTPS vertical-slice release gates are green;
- network, services, security, recovery, update, and reset proof owners are
  named in the final matrix;
- branch CI and post-merge main CI are green;
- the roadmap plan marks ITEM-334 through ITEM-337 complete.

## Non-Goals

- No new kernel, boot, storage, network, service, management, security,
  recovery, update, reset, or release behavior.
- No new QEMU proof or new test ISO.
- No production route, vector, policy, or publication activation.
- No release version change.
