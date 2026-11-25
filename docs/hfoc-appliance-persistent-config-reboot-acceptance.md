# HFOC Appliance Persistent Configuration Reboot Acceptance

**Date:** 2026-09-06
**Target:** ITEM-335 of
`docs/superpowers/plans/2026-09-03-hfoc-appliance-platform.md`
**Base SHA:** `d59934a7c4a8d13774b015784461362d3eea05ce`
**Status:** Completed by ITEM-335

## Purpose

This document is the authoritative ITEM-335 persistent configuration reboot
acceptance boundary. ITEM-335 proves that a committed `system.hostname` value
survives a QEMU reboot, is reloaded correctly by a fresh kernel boot, and
that a checksum-valid but schema-invalid journal record follows the ITEM-304
safe-default recovery path on the next boot.

HFOC OS has no general writable filesystem and no approved production disk
driver. The reboot proof uses a deterministic macro-gated QEMU fixture:
QEMU loads a 16 KiB RAM storage file into a reserved physical region before
CPU start, and the host saves that region back after each phase. Each phase
is a separate QEMU process, so this is a real reboot sequence rather than a
fresh in-memory load in the same kernel boot.

## Storage Fixture

The macro-gated `HFOC_APPLIANCE_PERSISTENT_CONFIG_QEMU_TEST` translation unit
reads physical RAM at `0x1f000000` through the VMM HHDM direct map. The
harness layout is:

| Offset | Field |
| --- | --- |
| `0x0000` | 8-byte `HFOCREBT` magic |
| `0x0008` | reboot state byte: `1`, `2`, or `3` |
| `0x1000` | 4 KiB `HFOCDATA` block-device backing store |

The backing store uses the existing checksummed `config_journal` and
`HFOCDATA` data-region contracts. No real disk, ATA, AHCI, NVMe, virtio-blk,
or PCI block-device driver is introduced.

## Reboot State Machine

The reboot-aware path runs before the legacy persistent-config proof and
returns without running that proof when the `HFOCREBT` fixture is present:

| State | Boot behavior | Marker |
| --- | --- | --- |
| `1` | install empty data region; publish default; commit `appliance-335` | `[REBOOT-CONFIG] phase1 committed` |
| `2` | load committed `appliance-335`; append schema-invalid hostname record | `[REBOOT-CONFIG] phase2 invalid committed` |
| `3` | load invalid record; publish `DEFAULT_INVALID` safe default | `[REBOOT-CONFIG] phase3 recovery OK` |

Each state is written only after the journal operation succeeds. An unknown
or truncated fixture fails closed with `[REBOOT-CONFIG] failed:` and stops.

## QEMU Acceptance

`make test-persistent-config-reboot-acceptance-qemu` runs
`scripts/test-qemu-persistent-config-reboot-acceptance.sh` against the
existing persistent-config test ISO with q35, `-smp 1`, no custom
`QEMU_CPU`, and the same bounded QEMU profile as existing acceptance:

1. Boot 1 verifies an empty store publishes the safe default, commits
   `appliance-335`, and emits `phase1 committed`.
2. Boot 2 verifies a fresh kernel load sees source `COMMITTED` and hostname
   `appliance-335`, appends the invalid record, and emits
   `phase2 invalid committed`.
3. Boot 3 verifies the recovery source is `DEFAULT_INVALID`, the hostname is
   `hfoc-appliance`, and emits `phase3 recovery OK`.
4. The proof rejects kernel panics and `[REBOOT-CONFIG] failed:` output.
5. QEMU exits cleanly through HMP after every phase.

## Production Boundary

The reboot fixture is compiled only into the macro-gated persistent-config
test kernel and ISO. Production kernel and production ISO contain no
`[REBOOT-CONFIG]` marker and no reboot-aware storage path. Normal production
boot behavior is unchanged.

## Future Ownership

- ITEM-336: management plane QEMU acceptance.
- ITEM-337: final HFOC Appliance Platform release acceptance.

This item does not activate those owners or change storage, network, service,
HTTPS, auth, authorization, recovery, factory-reset, or normal-boot
activation.

## Non-Goals

- No production normal-boot state loader or real storage discovery.
- No general writable filesystem or disk driver.
- No hostname editing command, management route, authentication, or service
  activation.
- No QEMU `system_reset` reliance on unverified RAM retention.
- No release version change.
