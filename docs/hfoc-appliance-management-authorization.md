# HFOC Appliance Management Authorization

**Date:** 2026-09-05
**Target:** ITEM-321 of
`docs/superpowers/plans/2026-09-03-hfoc-appliance-platform.md`
**Base SHA:** `ce7e6430154ab89c629066f293fbbb4babdd2fd9`
**Status:** Completed by ITEM-321

## Purpose

This document is the authoritative ITEM-321 management authorization
boundary. ITEM-319 fixed the management authentication architecture and
ITEM-320 implemented the bounded administrative authentication module.
ITEM-321 now adds the role/permission enforcement stage that separates
authentication from route dispatch: an allocation-free policy classifies the
exact HTTPS management route and checks the authenticated administrative
principal's fixed role/capability claims through the existing
`authorization_matrix`.

The Wave D HTTPS management API remains unchanged: routes are read-only,
advertise `auth=unavailable`, and are not wired through this authorization
policy yet. ITEM-322 owns secret and credential derivation/protection before
Wave E protected routes are published.

## Current State

The repository contains:

- the ITEM-319 management authentication architecture;
- the ITEM-320 `appliance_admin_auth` boundary with one immutable local
  `admin` principal, opaque bearer sessions, and exact request
  authentication;
- read-only `/api/v1`, `/api/v1/system`, `/api/v1/network`,
  `/api/v1/network/status`, and `/api/v1/service` Wave D routes;
- the existing allocation-free `authorization` matrix used by local-control
  boundaries;
- no HTTPS management authorization boundary, no exact route-policy table,
  and no stable classification between authentication and route dispatch.

## Security Decisions

1. Authentication and authorization remain separate stages. Authorization
   consumes only a validated `appliance_admin_auth_decision` and never accepts
   role or capability claims from an HTTP request.
2. Appliance v1 has one administrative principal. Protected read routes
   require the fixed `AUTHORIZATION_ROLE_ADMIN` role and
   `AUTHORIZATION_CAPABILITY_STATUS_READ` capability.
3. The policy recognizes only exact current management routes. Unknown
   `/api/*` targets and administrative actions without a published matrix
   entry are denied by default.
4. The policy is allocation-free, performs no storage I/O, audit write,
   secret handling, blocking, or interrupt-context access.
5. The policy returns bounded statuses that a future route dispatcher can map
   to stable external semantics: missing/invalid authentication becomes
   `401 Unauthorized`; an authenticated principal denied by authorization
   becomes `403 Forbidden`. This item does not publish those HTTP responses.

## Route Policy

The default policy published by
`appliance_management_authorization_policy_init()` contains four immutable
`authorization_matrix` entries:

| Route | Path | Method | Requirement |
| --- | --- | --- | --- |
| Discovery | `/api/v1` | `GET`, `HEAD` | public |
| System status | `/api/v1/system` | `GET`, `HEAD` | admin + status-read |
| Network configuration | `/api/v1/network` | `GET`, `HEAD` | admin + status-read |
| Network status | `/api/v1/network/status` | `GET`, `HEAD` | admin + status-read |
| Service status | `/api/v1/service` | `GET`, `HEAD` | admin + status-read |

No mutating administrative action has a matrix entry in the default policy.
An exact future mutation route must be added by an approved focused design
before Wave E publication.

## Authorization Decision Semantics

`appliance_management_authorization_check_request()` validates the
administrative authentication decision before dispatch:

- an unauthenticated decision must have no principal/session/generation
  claims and may reach only public discovery;
- an authenticated decision must name principal ID `1`, have a valid nonzero
  session ID, and have a valid nonzero credential generation;
- a protected read route requires an authenticated admin decision followed by
  a successful authorization-matrix check;
- missing role and missing capability results map to explicit
  `DENIED_ROLE` and `DENIED_CAPABILITY` statuses;
- any other malformed policy/matrix state denies the request with
  `BAD_AUTHORIZATION` or `BAD_STATE`;
- unsupported methods on exact routes return `METHOD_NOT_ALLOWED`, and unknown
  management targets return `UNKNOWN_ROUTE`.

The module never exposes credentials, session tokens, role bytes, or
capability bytes through status strings or route strings.

## Wave E Boundaries

- ITEM-319: management authentication architecture.
- ITEM-320: administrative authentication implementation.
- ITEM-321: management authorization and route policy.
- ITEM-322: secret and credential protection implementation.
- Controlled network/service mutation only after ITEM-321 completes Wave E.

The existing management API continues to advertise `auth=unavailable`. No
normal-boot, HTTPS, storage, audit, or hardware transition is introduced by
this item.

## Non-Goals

- No protected HTTPS route publication or HTTP `401`/`403` response change.
- No mutation API, local-control authorization change, credential
  derivation, at-rest protection, rotation policy, or production secret
  namespace.
- No persistent session storage or route-dispatch integration.
- No normal-boot administrative authorization activation.
- No QEMU proof, because this item changes no observable boot or HTTPS route
  behavior.
- No release version change.

## Acceptance

The focused host fixture in
`tests/appliance-management-authorization-host.c` proves exact route
classification, public discovery, unauthenticated denial, authenticated read
authorization, missing-role denial, missing-capability denial, default denial
for unknown/unsupported actions, exact status/route strings, and no
allocation. The fixture is part of `make test` through the canonical host
suite.
