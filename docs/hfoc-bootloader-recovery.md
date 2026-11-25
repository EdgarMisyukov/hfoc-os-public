# HFOC Bootloader Recovery

**Date:** 2026-09-06
**Target:** ITEM-342 of
`docs/superpowers/plans/2026-09-06-hfoc-appliance-installer-and-management-ui.md`
**Base SHA:** `758ee0ae265427b2604e4396ce8668e8be669029`
**Status:** Completed by ITEM-342

## Purpose

This document is the authoritative ITEM-342 bootloader recovery boundary.
ITEM-342 implements deterministic recovery evidence, scenario assessment,
plan preview/confirmation, and outcome validation for HFOC bootloader
problems. It does not mutate EFI files or NVRAM because the repository still
has no EFI filesystem or UEFI variable driver.

The module is the recovery-control seam for a future installer/recovery
media implementation.

## Module

`installer_boot_recovery` lives under `kernel/` and publishes:

- bounded recovery evidence;
- fail-closed scenario assessment;
- deterministic recovery plans;
- bounded ASCII previews;
- explicit confirmation state;
- repair-outcome validation;
- exact status strings.

No API writes EFI files, UEFI variables, block devices, or existing-OS boot
state.

## Evidence

Recovery evidence must state firmware mode, target disk, ESP presence, HFOC
bootloader presence, HFOC EFI entry presence, recovery media availability,
previous boot-configuration availability, and existing-OS boot-path
preservation. Missing evidence fails closed.

## Recovery Scenarios

The module supports:

- damaged HFOC bootloader;
- lost HFOC EFI entry;
- failed bootloader update;
- HFOC boot restoration;
- existing-OS preservation.

Each scenario maps to required mutation flags and recovery preconditions.
The recovery path must be non-empty and available.

## Confirmation And Outcome

A plan publishes a preview before confirmation. Confirmation changes only
plan state. Outcome validation reports `OK` only after the caller confirms
HFOC bootloader presence, HFOC EFI entry presence, and existing-OS boot-path
preservation according to the plan.

## Host Proof

`make test-installer-boot-recovery-host` runs deterministic host fixtures for
each recovery scenario, unavailable recovery media, missing previous
boot configuration, unprotected existing OS, unconfirmed plans, and
incomplete repair outcomes.

This is a host proof, not physical-machine bootloader recovery.

## Boundaries

- ITEM-338 owns the installer architecture audit.
- ITEM-339 owns read-only hardware and existing-OS discovery.
- ITEM-340 owns clean layout and partition transaction semantics.
- ITEM-342 (this item) owns bootloader recovery evidence and plans.
- ITEM-346 owns integrated installer/boot recovery acceptance.

## Non-Goals

- No EFI filesystem read or write.
- No UEFI NVRAM mutation.
- No Limine disk stage installation.
- No boot of a repaired HFOC or existing OS in this item.
- No real disk-controller driver.
- No physical-hardware acceptance.
- No VERSION or release-number change.
