# HFOC Integrated Installer, Dual-Boot And Management Acceptance

**Date:** 2026-09-06
**Target:** ITEM-346 of
`docs/superpowers/plans/2026-09-06-hfoc-appliance-installer-and-management-ui.md`
**Base SHA:** `c00b152eb380d71c6e99da07ee345530116924a3`
**Prerequisite boundaries:** ITEM-338..345; ITEM-334..337 platform
acceptance gates
**Status:** Active by ITEM-346

## Purpose

This document is the authoritative ITEM-346 integrated installer,
dual-boot and management acceptance boundary. ITEM-346 records the
deterministic software proof inventory that covers each installer and
management boundary already merged into `main`, and fixes the release
requirement that physical-machine installation and management evidence
cannot be replaced by QEMU or host proofs.

The current codebase has no production installer subsystem, no production
EFI filesystem/NVRAM mutation, no production normal-boot first-boot wizard
loader, and no production normal-boot management loader. ITEM-346 therefore
creates an honest acceptance gate and leaves the corresponding physical
checks `NOT_TESTED` until separate Hardware Acceptance Report evidence is
recorded.

## Software Proof Matrix

`release/hfoc-integrated-installer-dual-boot-management-acceptance.txt` is
the canonical software proof inventory. It names the exact target and log
marker owned by each deterministic proof.

| Category | Target | Marker |
| --- | --- | --- |
| `ARCHITECTURE_AUDIT` | `test-item-338-installer-architecture-audit-host` | `Item 338 installer architecture audit test: OK` |
| `HARDWARE_DISCOVERY` | `test-installer-hardware-discovery-host` | `Installer discovery host test: OK` |
| `INSTALLER_LAYOUT` | `test-installer-layout-host` | `Installer layout host test: OK` |
| `DUAL_BOOT` | `test-installer-dual-boot-host` | `Installer dual boot host test: OK` |
| `BOOT_RECOVERY` | `test-installer-boot-recovery-host` | `Installer boot recovery host test: OK` |
| `FIRST_BOOT_WIZARD` | `test-installer-first-boot-wizard-host` | `Installer first boot wizard host test: OK` |
| `NORMAL_BOOT` | `test-integrated-appliance-boot-acceptance-qemu` | `Integrated appliance boot acceptance: OK` |
| `LOCAL_CONSOLE` | `test-appliance-boot-console-qemu` | `QEMU appliance boot console test: OK` |
| `HTTPS_MANAGEMENT` | `test-management-plane-qemu` | `QEMU management plane acceptance test: OK` |
| `PERSISTENT_CONFIG` | `test-persistent-config-reboot-acceptance-qemu` | `Persistent configuration reboot acceptance: OK` |
| `NETWORK_CONFIG` | `test-appliance-network-config-qemu` | `QEMU network config test: OK` |
| `POWER_CYCLE` | `test-power-cycle-continuity-drill-host` | `Power-cycle continuity drill host tests: ALL OK` |
| `FAILED_NETWORK` | `test-network-partition-drill-host` | `Network partition drill host tests: ALL OK` |
| `ABNORMAL_RESTART` | `test-https-service-lifecycle-host` | `HTTPS graceful stop and restart host test: ALL OK` |
| `RECOVERY_MODE` | `test-appliance-recovery-mode-host` | `appliance recovery mode host: OK` |
| `FACTORY_RESET` | `test-appliance-factory-reset-host` | `appliance factory reset host: OK` |

The matrix is a software acceptance inventory. QEMU and host proofs are not physical-hardware acceptance.

## Physical Checks That Remain Not Tested

Until a dedicated physical-machine Hardware Acceptance Report records
`PASS`, the following checks must remain NOT_TESTED:

- UEFI and legacy firmware installer boot;
- clean physical disk installation;
- physical Linux installation preservation and existing-OS boot;
- physical Windows installation preservation and existing-OS boot;
- physical boot-menu selection;
- UEFI boot entry creation and preservation;
- bootloader failure and recovery on physical storage;
- physical first-boot configuration and reboot;
- physical DHCP/static IP/gateway/DNS/HTTPS operation;
- physical power cycle and persistent-state survival;
- physical factory reset;
- production browser management-plane operation.

QEMU/host success for a boundary does not convert any of these checks to
`PASS`. A release cannot be declared HFOC-APPLIANCE-V1 complete until the
real-hardware report supplies the required evidence.

## Gate

`scripts/verify-hfoc-integrated-installer-dual-boot-management-acceptance.sh`
validates matrix shape, exact category order, uniqueness, Makefile targets,
real proof markers, and this document. The runner
`scripts/test-hfoc-integrated-installer-dual-boot-management-acceptance.sh`
prints:

```text
HFOC integrated installer/dual-boot/management acceptance gate: OK
```

`make test-hfoc-integrated-installer-dual-boot-management-acceptance-host`
runs the host fixture and
`make test-hfoc-integrated-installer-dual-boot-management-acceptance` runs
the real gate. `make test` invokes the real gate after the existing ITEM-337
platform release gate.

## Completion

ITEM-346 is complete when:

- ITEM-338 through ITEM-345 and the referenced platform proofs are merged;
- the canonical matrix verifies and names only real proof owners;
- the host fixture rejects malformed, duplicate, or missing proof matrices;
- the active plan and metadata mark ITEM-346 complete and ITEM-347
  incomplete;
- `make check`, `make iso`, focused proofs, branch CI, and post-merge CI are
  green.

Completion of ITEM-346 does not imply real-hardware acceptance and does not
make HFOC-APPLIANCE-V1 releasable.

## Boundaries

- ITEM-338 owns installer architecture audit.
- ITEM-339 owns hardware and OS discovery.
- ITEM-340 owns clean installer layout.
- ITEM-341 owns dual-boot planning and boot-menu records.
- ITEM-342 owns bootloader recovery evidence.
- ITEM-343 owns first-boot wizard control.
- ITEM-344 owns local appliance console presentation.
- ITEM-345 owns HTTPS management web UI presentation.
- ITEM-346 (this item) owns integrated installer/dual-boot/management
  acceptance inventory and physical-machine procedure boundary.
- ITEM-347 owns installation, administration, recovery and hardware
  documentation.

## Non-Goals

- No production installer, EFI filesystem, UEFI NVRAM, or bootloader
  mutation.
- No production normal-boot first-boot wizard or management loader.
- No physical-hardware or production-browser acceptance claim.
- No replacement for the ITEM-337 platform release gate.
- No VERSION or release-number change.
