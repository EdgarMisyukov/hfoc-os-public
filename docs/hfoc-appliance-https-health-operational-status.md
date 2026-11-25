# HFOC HTTPS Health And Operational Status

**Date:** 2026-09-06
**Target:** ITEM-328 of
`docs/superpowers/plans/2026-09-03-hfoc-appliance-platform.md`
**Canonical design:** `docs/superpowers/specs/2026-09-06-item-328-https-health-operational-status-design.md`

## Purpose

ITEM-328 adds the read-only HTTPS operational status/health API route
`/api/v1/health`. It publishes the ITEM-326 aggregate appliance health model
as JSON on the macro-gated HTTPS proof surface without introducing a
production normal-boot listener or bypassing the Wave E authentication,
authorization, and secret-protection boundary.

## Route

The route is exact and allocation-free:

```text
GET  /api/v1/health
HEAD /api/v1/health
POST /api/v1/health -> 405 Method Not Allowed
```

Successful `GET` and `HEAD` return `200 OK`, `Content-Type: application/json`,
and bounded JSON fields:

- `api`
- `version`
- `name`
- `status`
- `auth`
- `mutating`
- `state`
- `healthy`
- `critical`
- `first_fault_domain`
- `first_fault`

The module is allocation-free, read-only, deny-by-default, and stateless. It
performs no storage I/O, runtime-log append, audit write, secret handling,
or normal-boot change.

## Boundaries

The health API boundary is a management status boundary, not a health
collector. It accepts a caller-owned validated `appliance_health_input`
snapshot and publishes the aggregate `appliance_health_output` returned by
ITEM-326.

- Only `GET` and `HEAD` succeed.
- No controlled operation or mutation endpoint is published.
- Authentication remains `unavailable` until ITEM-319 implementation.
- Future controlled update/recovery work remains with ITEM-329 onward.

## Usage Boundary

This document is the authoritative appliance operational status boundary for
ITEM-328. Related boundaries are:

- `docs/hfoc-appliance-health-model.md` (ITEM-326)
- `docs/hfoc-appliance-runtime-diagnostics-logs.md` (ITEM-327)
- `docs/superpowers/specs/2026-09-06-item-328-https-health-operational-status-design.md`

## Files

- `kernel/include/appliance_health_api.h`
- `kernel/src/appliance_health_api.c`
- `tests/appliance-health-api-host.c`
- This authoritative document
