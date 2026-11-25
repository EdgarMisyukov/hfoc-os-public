# HFOC Appliance Recovery Mode

**Date:** 2026-09-06
**Target:** ITEM-332 of
`docs/superpowers/plans/2026-09-03-hfoc-appliance-platform.md`
**Configuration boundary:**
`docs/hfoc-appliance-configuration-integrity.md` (ITEM-324)
**Update boundaries:**
`docs/hfoc-appliance-update-package-architecture.md` (ITEM-329),
`docs/hfoc-appliance-update-manifest-validation.md` (ITEM-330), and
`docs/hfoc-appliance-update-integrity-verification.md` (ITEM-331)
**Status:** Completed by ITEM-332

## Purpose

This document is the authoritative ITEM-332 appliance recovery-mode boundary.
ITEM-332 defines one minimal allocation-free recovery-decision path for failed
configuration, update, and runtime states. The path remains usable when normal
HTTPS management services are unavailable because it evaluates only
caller-owned evidence and performs no service, storage, network, or remote
management work.

Existing lower-layer policy boundaries already prove the recovery primitives:

- `appliance_config_integrity_recovery_plan()` selects last-known-good,
  safe-default, or explicit-error recovery for configuration records;
- `recovery_trust` verifies recovery-purpose entry/exit and restricts actions
  to read-only evidence while recovery is active;
- `first_boot_health` and `automatic_rollback` handle failed first-boot
  candidate health;
- service failure handling and `appliance_health` provide bounded contained
  restart and read-only health evidence.

ITEM-332 adds the authoritative policy that combines those caller-owned
evidence classes into deterministic recovery decisions. It does not activate
a production recovery console, boot-slot mutation, storage writer, remote
route, or normal-boot state loader; those integrated paths remain owned by
ITEM-333, ITEM-334, and ITEM-335.

## Recovery Model

The recovery module recognizes exactly three failure domains:

| Domain | Decision source |
| --- | --- |
| `CONFIGURATION` | ITEM-324 configuration integrity planner |
| `UPDATE` | ITEM-331 verified update, first-boot health, automatic rollback |
| `RUNTIME` | service failure handling and read-only health evidence |

The evidence structure is:

```c
struct appliance_recovery_mode_evidence {
    uint32_t version;
    uint32_t domain;
    enum appliance_config_integrity_recovery config_recovery;
    bool update_verified;
    bool pending_update;
    bool first_boot_health_failed;
    bool restart_available;
    bool recovery_trust_active;
};
```

All flags are caller-owned booleans. The module never reads or writes a
subsystem state object while evaluating evidence.

## Decision Semantics

The decision set is bounded and exact:

- `NONE`: no recovery action is authorized by the supplied evidence.
- `KEEP_LAST_KNOWN_GOOD`: use an older valid configuration value.
- `SAFE_DEFAULT`: use the appliance safe default when no known-good value is
  available.
- `AUTOMATIC_ROLLBACK`: select the pending rollback after a failed first-boot
  health contract.
- `SUPERVISED_RESTART`: permit a bounded local service restart without remote
  management.
- `READ_ONLY`: publish or retain only diagnostic/status evidence.

`UPDATE` evidence maps to `AUTOMATIC_ROLLBACK` only when the update is
verified, a pending update exists, and first-boot health failed. Without all
three conditions the update domain maps to `READ_ONLY`, never to an unapproved
mutation. `CONFIGURATION` maps according to the ITEM-324 recovery-plan result.
`RUNTIME` maps to `SUPERVISED_RESTART` only while restart is available;
exhaustion maps to `READ_ONLY`.

Every decision is local-only. `AUTOMATIC_ROLLBACK` is the only decision that
requires reboot. An active `recovery_trust` evidence flag overrides any
mutating decision to `READ_ONLY`; it never bypasses the trusted recovery gate.

## API

`kernel/include/appliance_recovery_mode.h` publishes:

- `APPLIANCE_RECOVERY_MODE_VERSION`;
- bounded domain and action constants;
- exact status values and status strings;
- `struct appliance_recovery_mode_evidence`;
- `struct appliance_recovery_mode_decision`;
- `struct appliance_recovery_mode_gate`;
- `appliance_recovery_mode_init()`;
- `appliance_recovery_mode_evaluate()`;
- `appliance_recovery_mode_is_evaluated()`;
- domain/action/status string helpers.

`appliance_recovery_mode_init()` publishes a clean one-shot gate.
`appliance_recovery_mode_evaluate()` validates version, domain, and
configuration-plan evidence, maps to a decision, applies recovery-trust
override if needed, and publishes the decision and gate atomically.

## Gate Semantics

The gate is a one-shot recovery-decision record. It owns no state object,
storage, service, boot slot, or management dependency. Reinitialization and
reuse of an evaluated gate are rejected. Every failed evaluation leaves the
caller decision and gate byte-for-byte unchanged.

## Validation And Failure

- Null gate, evidence, or decision output is rejected.
- Unknown version, domain, or `config_recovery` value is rejected.
- `DOMAIN_NONE` cannot publish a recovery decision.
- An uninitialized or already evaluated one-shot state is rejected.
- The module performs no allocation, blocking, storage I/O, audit write,
  diagnostic output, locking, or interrupt-context access.
- No raw credentials, private keys, tokens, secrets, shell commands, or file
  paths ever appear in recovery evidence, decisions, or this module.
- No release, tag, VERSION, management route, or normal-boot activation is
  introduced.

## Files

- `kernel/include/appliance_recovery_mode.h`
- `kernel/src/appliance_recovery_mode.c`
- `tests/appliance-recovery-mode-host.c`
- `docs/superpowers/specs/2026-09-06-item-332-appliance-recovery-mode-design.md`
- `docs/superpowers/plans/2026-09-06-item-332-appliance-recovery-mode-implementation.md`
- This authoritative document
