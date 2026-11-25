# HFOC Recovery Guide

**Release ID:** HFOC-APPLIANCE-V1
**Scope:** Bootloader recovery, recovery mode, and factory reset

## Purpose

HFOC provides a recovery path for damaged bootloader state, lost EFI
entries, failed bootloader updates, and unsafe appliance state while
preserving existing operating systems. Physical recovery procedures remain
`NOT TESTED` until the Hardware Acceptance Report records dedicated-machine
evidence.

## Bootloader Recovery

Bootloader recovery must:

1. Detect the damage scenario without modifying existing OS boot state.
2. Show a preview and require explicit confirmation before repair.
3. Repair the HFOC boot path or lost EFI entry.
4. Validate that HFOC and existing OS boot paths remain available.

Do not delete unrelated UEFI entries or reformat the shared ESP.

## Recovery Mode

Recovery mode is a read-only trust boundary used when normal management
services are unavailable. First-boot and destructive mutations must not
start while recovery trust is active.

## Factory Reset

Factory reset clears the correct persistent appliance state while preserving
the minimum boot/recovery foundation required to restart safely. It is a
destructive operation and requires explicit confirmation.

## Existing OS Preservation

Recovery must never damage existing Linux or Windows boot paths. Before
repair, record the previous boot configuration and existing OS evidence.
After repair, verify every preserved boot path.

## Acceptance

Host proofs cover recovery evidence, repair plans, recovery-mode trust, and
factory reset boundaries. Physical bootloader recovery and factory reset
remain NOT TESTED until Hardware Acceptance Report evidence is recorded.

## Non-Goals

- No UEFI mutation without evidence and confirmation.
- No destructive operation without a safety preview.
- No physical recovery acceptance claim.
- No release version change.
