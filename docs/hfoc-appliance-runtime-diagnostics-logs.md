# HFOC Appliance Runtime Diagnostics And Logs

**Date:** 2026-09-05
**Target:** ITEM-327 of
`docs/superpowers/plans/2026-09-03-hfoc-appliance-platform.md`
**Canonical design:** `docs/superpowers/specs/2026-09-05-item-327-runtime-diagnostics-logs-design.md`

## Summary

ITEM-327 adds the bounded runtime log/diagnostics boundary for HFOC
appliance operations. The module publishes an allocation-free structured
ring of fixed-size log entries that callers can append, pop, and snapshot
without string formatting, filesystem writes, allocation, or secret payloads.

Each entry carries:

- auto-assigned sequence and caller-mode tick value;
- exact severity;
- exact component category;
- bounded operation and result values;
- optional bounded fault/context detail fields;
- a bounded opaque numeric detail.

The boundary is intentionally numeric and deny-by-default. There is no
free-form text argument, no raw private/session/credential byte path, and
no diagnostic namespace persistence. ITEM-328 adds the HTTPS operational status route in `docs/hfoc-appliance-https-health-operational-status.md`; ITEM-329
status; ITEM-334 owns integrated boot acceptance.

## Record Model

`appliance_runtime_log_record` is the caller-owned record submitted to the
log. `appliance_runtime_log_entry` is the stored entry returned by snapshots
and pops; it adds only the assigned sequence.

The exact valid values are:

| Field | Allowed |
| --- | --- |
| severity | `DEBUG`, `INFO`, `WARN`, `ERROR`, `CRITICAL` |
| category | `BOOT`, `STORAGE`, `CONFIGURATION`, `NETWORK`, `SERVICE`, `HTTPS_MANAGEMENT`, `MANAGEMENT`, `SECURITY`, `PROCESS` |
| operation | `1..255` |
| result | `0..255`; zero means success in existing HFOC status style |
| fault | `0..65535`; zero means none |
| context_generation | `0..16777215` |
| detail | `0..4294967295`; opaque caller failure/context code |

Unknown values are rejected before publication and never mutate the log.

## Bounded Log

`struct appliance_runtime_log` is caller-owned and must be initialized before
append/pop/snapshot/state operations:

- capacity is fixed at 64 entries;
- `published` is false until `appliance_runtime_log_init()` succeeds;
- sequences start at zero and increase by one on each successful append;
- append returns `FULL` at capacity; it never silently overwrites older
  entries;
- snapshot is non-destructive and returns `OVERFLOW` when the destination is
  too small;
- pop returns `EMPTY` when no entry is available and otherwise drains oldest
  entry first;
- failed operations do not change count, read cursor, write cursor, sequence,
  or destination outputs.

The implementation performs no allocation, storage I/O, serial/console
output, audit append, blocking, locking, or interrupt-policy change. A caller
may use the same storage shape as an interrupt-safe bounded ring only if it
already serializes calls; the module itself does not lock.

## Usage Boundary

This module is a policy/publish boundary, not a boot-publisher. No production
kernel boot path, storage journal, console command, metric path, HTTPS route,
or health evaluator changes in this item. Future operational consumers may
call `appliance_runtime_log_append()` from their owners and drain via
`appliance_runtime_log_snapshot()`.

No raw credential, token, private key, session, address, domain name, or
other secret-bearing bytes are accepted by the log API. Diagnostic context is
limited to numeric enum/code fields so release and operational layers cannot
accidentally export raw secret material.

## Files

- `kernel/include/appliance_runtime_log.h`
- `kernel/src/appliance_runtime_log.c`
- `tests/appliance-runtime-log-host.c`
- This authoritative document
