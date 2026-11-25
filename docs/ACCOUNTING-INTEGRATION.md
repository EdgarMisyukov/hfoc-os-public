# External HFOC Accounting Integration Boundary

This document is the authoritative item 171 boundary for HFOC OS and the
external HFOC Accounting application. It fixes what each side owns, how audit
hand-off works, what transport/versioning rules apply, and which operations
are never supported by the appliance.

## Contract Identity

The integration contract schema is `HFOC-ACCT-INT-1` at version 1. The
appliance accepts exactly version 1 until an approved focused item defines a
new version. A peer with an unknown or unsupported version fails closed and
never obtains a live session.

## Ownership Split

### HFOC OS Owns Appliance State

HFOC OS owns all appliance state and behavior, including:

- kernel/runtime ownership, memory, descriptors, processes, and services;
- bounded storage, block-device, data-region, journal, checkpoint, backup,
  restore, and recovery state;
- appliance configuration and controlled configuration transactions;
- health, metrics, status, support-bundle manifest/export, and diagnostic
  redaction;
- audit/event generation and retention under existing ring and privacy
  policy;
- local management, network admission, trust, image, update, and recovery
  policy boundaries.

### HFOC Accounting Owns Accounting State

The external HFOC Accounting application owns all accounting state and
behavior, including:

- ledger entries, chart of accounts, balances, tax, documents, and payroll;
- derived accounting data and accounting storage/lifecycle;
- processing and retention of exported hand-off data after it leaves the
  appliance boundary;
- all decisions and side effects required by accounting business rules.

### Shared Integration Boundary

The shared integration boundary owns only the narrow external contract:

- version negotiation and peer identity;
- accounting hand-off envelope framing;
- accepted-envelope identity metadata;
- ordered, acknowledged audit/event export cursor;
- locally authorized, validated, staged, and audited configuration intents;
- supported protocol/schema version range and deprecation state.

These appliance-side contract pieces must never become accounting business
logic.

## Integration Principal Scope

External HFOC Accounting peers use the dedicated integration principal role
`AUTHORIZATION_ROLE_INTEGRATION` (`4`) and the integration session capability
`AUTHORIZATION_CAPABILITY_INTEGRATION_SESSION` (`4`). A valid
`struct integration_principal_scope` has version 1 and exactly those IDs.
The scope is distinct from local admin, operator, auditor, status-read,
config-write, and audit-read authorization. It is denied by the existing
authorization matrix for local-admin-only status and controlled-configuration
commands; it cannot override or bypass local management.

## Audit Hand-off

HFOC OS generates and retains bounded audit/event records. The appliance may
expose an ordered, acknowledged export cursor for integration peers, but it
never grants direct audit mutation or redaction bypass. The external
accounting application may copy and process exported records; it cannot alter
appliance audit state, suppress event loss, or read secret-bearing fields.

## Transport

Integration traffic uses the existing deny-by-default management transport
and admission policy unless an approved focused item changes that boundary.
There is no arbitrary socket, listener, routing daemon, cloud dependency, or
open external surface beyond the narrow versioned accounting hand-off
contract.

## Versioning

- Contract schema: `HFOC-ACCT-INT-1`.
- Supported schema versions: exactly 1.
- Supported transport versions: exactly 1.
- Unknown versions are rejected before session, envelope, or command
  processing.
- Future versions require a focused design spec and implementation plan.

## Unsupported Operations

The integration boundary explicitly rejects:

- arbitrary shell or command execution;
- direct appliance state/configuration mutation;
- direct audit-state mutation or diagnostic redaction bypass;
- local-admin-only command override;
- accounting ledger, balance, chart-of-accounts, tax, document, or payroll
  operations inside HFOC OS;
- external peer access to memory, process, storage, or trust ownership;
- any accounting business logic in this repository.

## Integration Session Handshake

Item 173 adds a bounded appliance-side integration session handshake. A valid
request carries the exact `HFOC-ACCT-INT-1` schema version 1, transport
version 1, a peer identity in `1..65535`, and a non-zero 32-byte nonce. The
caller-owned bounded nonce registry records one nonce per peer and rejects a
replayed peer nonce with an explicit rejection code. Schema mismatch,
transport mismatch, invalid peer identity, zero nonce, corrupted registry,
and registry-full states fail closed without changing registry bytes.

Run the focused handshake proof with:

```bash
make test-integration-session-host
```

The module performs no allocation, blocking, storage or network I/O,
diagnostic output, audit write, credential storage, or interrupt-policy
change. Items 174 through 177 own the envelope, ledger, cursor, and
configuration-intent boundaries; item 178 owns failure isolation; items 179
and 180 own compatibility and Wave R QEMU proof.

## Accounting Hand-off Envelope v1

Item 174 adds a bounded appliance-side accounting hand-off envelope. A valid
envelope carries the deterministic 16-byte ID `HFOCACCTHANDOFF1`, envelope
version 1, the item 171 schema version 1, audit-hand-off purpose, a non-zero
32-byte correlation ID, a payload length in `1..4096`, zeroed reserved header
bytes, and a SHA-256 digest over the header and payload. `seal()` produces the
deterministic framing; `validate()` rejects malformed IDs, unsupported
versions, invalid purposes, corrupted reserved bytes, zero correlation IDs,
invalid or inconsistent payload lengths, and digest mismatches without
publishing a layout.

Run the focused envelope proof with:

```bash
make test-integration-envelope-host
```

The module performs no allocation, blocking, storage or network I/O,
diagnostic output, audit write, credential storage, or interrupt-policy
change. Item 175 owns the accepted-envelope idempotency ledger; item 176 owns
the outbound event cursor; item 177 owns inbound configuration intents; and
item 178 owns integration failure isolation; items 179 and 180 own
compatibility and Wave R QEMU proof.

## Idempotency Ledger

Item 175 adds a bounded appliance-side accepted-envelope identity ledger. A
32-byte identity is the item 174 envelope SHA-256 digest. The caller prepares
an identity and result once, then atomically commits exactly that prepared
result. Replay returns the exact committed result without changing the
ledger; duplicate prepares and duplicate commits return the existing result
without appending a duplicate entry. A pending entry can be rolled back so
its bounded slot is reusable, while a committed identity cannot be changed or
removed. The caller-owned ledger holds at most 64 identities.

Run the focused idempotency proof with:

```bash
make test-integration-ledger-host
```

The module performs no allocation, blocking, storage or network I/O,
diagnostic output, audit write, credential storage, or interrupt-policy
change. Item 177 owns inbound configuration intents; item 178 owns integration
failure isolation; and items 179 and 180 own compatibility and Wave R QEMU
proof.

## Outbound Event Cursor

Item 176 adds a bounded appliance-side outbound audit/event export cursor. A
valid cursor holds version 1 and bounded capacity up to 64 pending records.
Export appends an exact ordered unacknowledged event and rejects a full
cursor. Acknowledge accepts only the oldest pending export sequence, removes
that record, and reclaims the slot. Snapshot returns only pending records in
export order with one mutation generation and is published only when the
generation is unchanged after the copy. Overflow and stale-generation
failures publish nothing.

Run the focused outbound cursor proof with:

```bash
make test-integration-event-cursor-host
```

The module performs no allocation, blocking, storage or network I/O,
diagnostic output, audit write, credential storage, or interrupt-policy
change. Item 177 owns inbound configuration intents; item 178 owns integration
failure isolation; and items 179 and 180 own compatibility and Wave R QEMU
proof.

## Inbound Configuration Contract

Item 177 adds a bounded appliance-side inbound configuration contract. A valid
intent carries version 1, peer identity in `1..65535`, one of exactly seven
narrow intent IDs, and a value inside that intent's exact schema bounds. The
intents cover `recovery_mode`, `max_processes`, `max_user_memory_pages`,
`audit_ring_capacity`, `boot_timeout_ms`, `restart_limit`, and
`service_health_timeout_ms`; schema version is never externally mutable.
`integration_config_intent_apply()` first requires local authorization through
the existing controlled-configuration command entry, then renders the exact
full candidate and delegates validation, staging, activation, rollback, and
audit publication to the item 128 controlled configuration boundary. The
dedicated integration principal scope is denied because it cannot satisfy the
local admin/config-write requirements. Invalid, denied, and rejected intents
leave active and pending configuration unchanged and publish bounded denial
evidence.

Run the focused inbound configuration proof with:

```bash
make test-integration-config-intent-host
```

The module performs no allocation, blocking, storage or network I/O,
diagnostic output, credential storage, or interrupt-policy change. Item 178
owns integration failure isolation; items 179 and 180 own compatibility policy
and the Wave R QEMU proof.

## Integration Failure Isolation

Item 178 adds a bounded appliance-side integration failure-isolation
boundary. A caller-owned table holds at most 64 external accounting peers and
records exactly malformed, unavailable, and slow peer faults. The first and
second consecutive failures publish a degraded peer; the third consecutive
failure publishes the peer as isolated for an exact cooldown of 10, 20, or 30
caller ticks depending on the fault kind. `integration_failure_isolation_check()`
is read-only and rejects an isolated peer only until its exact cooldown
deadline; unknown and recovered peers remain allowed.
`integration_failure_isolation_recover()` removes exactly one peer entry and
makes the slot reusable.
`integration_failure_isolation_local_allowed()` accepts exactly local
management, service supervision, and state recovery without consulting the
peer table, proving that a malformed, unavailable, or slow external peer can
never block those local paths.

Run the focused failure-isolation proof with:

```bash
make test-integration-failure-isolation-host
```

The module performs no allocation, blocking, storage or network I/O,
diagnostic output, audit write, credential storage, or interrupt-policy
change. Item 179 owns compatibility policy, and item 180 owns the Wave R QEMU
proof.

## Integration Compatibility Policy

Item 179 adds a bounded appliance-side compatibility policy. A valid policy
carries version 1, inclusive schema and transport version ranges derived from
item 171, and an explicit active or deprecated state. The current supported
schema and transport range is exactly version 1.

`integration_compatibility_policy_init()` publishes the exact active policy.
`integration_compatibility_policy_validate()` rejects bad versions, inverted
or out-of-range fields, and unknown deprecation states.
`integration_compatibility_policy_deprecate()` makes the policy deprecated
exactly once and does not allow reactivation.
`integration_compatibility_check()` is read-only and rejects unsupported
schema versions, unsupported transport versions, and every request while the
policy is deprecated.
`integration_compatibility_session_validate()` applies the compatibility gate
before the item 173 handshake, so an unsupported or deprecated peer never
obtains a live session and never mutates the session nonce registry. A
non-compatible handshake result is returned as `SESSION_REJECTED`.

Run the focused compatibility proof with:

```bash
make test-integration-compatibility-host
```

The module performs no allocation, blocking, storage or network I/O,
diagnostic output, audit write, credential storage, or interrupt-policy
change. Item 180 owns the Wave R QEMU proof.

## Accounting-boundary QEMU proof

Item 180 adds a macro-gated accounting-boundary QEMU proof. The proof
composes the Wave R integration contracts in one q35 / one-vCPU / default-CPU
session:

- exact `HFOC-ACCT-INT-1` schema identity and owner/unsupported-operation
  boundary;
- accepted handshake with exact schema/transport version 1 and replay
  rejection;
- unsupported schema/transport version rejection before live session
  publication;
- sealed/validated accounting hand-off envelope v1 and tampered-digest
  rejection;
- idempotent envelope identity prepare/commit/replay;
- integration config intent denial through the local controlled-configuration
  boundary with unchanged active/pending state;
- exact denied audit correlation and ordered outbound cursor export/
  acknowledgement;
- peer failure isolation after three malformed failures with unaffected local
  management, service supervision, and state recovery;
- active compatibility acceptance, unsupported-version rejection, and
  deprecated-policy gate;
- monitor-driven clean shutdown.

Run the proof with:

```bash
make test-accounting-boundary-qemu
```

`make test-accounting-boundary-qemu` builds a dedicated macro-gated ISO and
runs `scripts/test-qemu-accounting-boundary.sh`. The script requires every
proof marker exactly once, rejects proof failure and kernel panic, and
requires QEMU HMP clean quit. The normal production kernel and ISO contain no
`[ACCOUNTING-BOUNDARY]` markers.

## Verification

Run the focused proof with:

```bash
make test-integration-boundary-host
```

The test verifies exact contract schema, owner IDs, audit hand-off version,
transport version, and unsupported-operation bits. `make check` verifies that
the document, Makefile target, and queue metadata remain in force.

Run the focused integration principal proof with:

```bash
make test-integration-principal-host
```

The test verifies exact integration role/capability IDs, invalid scope
rejection, no-write publication, and denial of local-admin-only commands.
