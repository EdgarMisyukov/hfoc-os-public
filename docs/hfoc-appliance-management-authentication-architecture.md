# HFOC Appliance Management Authentication Architecture

**Date:** 2026-09-05
**Target:** ITEM-319 of
`docs/superpowers/plans/2026-09-03-hfoc-appliance-platform.md`
**Base SHA:** `b2ff6f9cee80cae679b2be7de6d3a5f2504de490`
**Status:** Completed by ITEM-319

## Purpose

This document is the authoritative architecture boundary for Wave E
management authentication. It defines the administrative principal model,
credential lifecycle, session semantics, request authentication semantics,
failure handling, and secure defaults that ITEM-320 through ITEM-322 must
implement.

The current HTTPS management API publishes read-only system, network, and
service status through Wave D boundaries and advertises `auth=unavailable`.
That mode remains correct until an administrative credential is initialized
and the Wave E authentication, authorization, and secret-protection modules
are implemented and proven. This document changes no runtime behavior.

## Current State

The repository already contains bounded local-control security primitives:

- local principal descriptors with immutable IDs, names, roles, and state;
- a versioned allocation-free credential verifier with fixed-length input;
- bounded local session IDs, generations, lifetimes, expiry, and close;
- per-principal failed-authentication throttle;
- authorization matrix and bounded role/capability checks;
- structured audit events with session/principal/command correlation;
- deny-by-default network admission and framed management transport.

The HTTPS management plane is separate from the local-control proof surface.
It currently serves:

- `/api/v1` version discovery;
- `/api/v1/system` read-only system state;
- `/api/v1/network` and `/api/v1/network/status` read-only network state;
- `/api/v1/service` read-only service state and aggregate health.

All management routes are `GET`/`HEAD`-only, advertise `auth=unavailable`
and `mutating=false`, and perform no credential handling or administrative
action. There is no production management authentication, no initialized
administrative credential, no remote session table, and no authenticated
request policy for the HTTPS management plane.

## Security Principles

The following principles are immutable for Wave E:

1. Management authentication for the HTTPS API is accepted only over the
   existing real TLS 1.3 HTTPS boundary. No plaintext HTTP administrative
   channel is added.
2. The management plane remains deny-by-default. Missing or invalid proof of
   authentication never reaches a protected management route.
3. Authentication and authorization are separate stages. Authentication
   proves principal identity; ITEM-321 enforces role/capability
   authorization.
4. No default administrative password exists. No first-boot credential is
   provisioned over the network.
5. Credentials are never stored or transmitted as plaintext and never write credentials or session secrets to logs, audit records, diagnostics,
   support bundles, or HTTP responses.
6. Authentication processing is bounded, allocation-free in the request
   path, and never performs storage I/O or interrupt-context work.

## Principal And Credential Model

The appliance publishes one immutable local `admin` principal for initial
administrative ownership:

- principal ID `1`;
- principal name `admin`;
- enabled state only after credential initialization;
- the existing `AUTHORIZATION_ROLE_ADMIN` role as the baseline role;
- no remote administrative principal creation in the Appliance v1 scope.

Existing local-control principals remain local proof/control fixtures unless
an approved future item explicitly binds them to production management.
ITEM-320 owns the administrative authentication implementation and must not
reuse a fixture-only principal set as the production management boundary.

Credential state is one of:

- absent: no initialized administrative credential exists;
- initialized: a bounded verifier value has been published locally;
- disabled: administrative authentication is intentionally denied;
- active: initialized and enabled, available for ITEM-320 onward;
- reset-pending: local reset requested before the next initialization.

Credential inputs and verifier values are bounded and fixed-length. The
verifier comparison is constant-time. Stored state never reconstructs the
original credential. ITEM-322 owns secret and credential protection
implementation, including derivation, at-rest protection, zeroization, and
rotation policy.

## Session Semantics

After ITEM-320 publishes authentication, authenticated requests carry an
opaque bearer session token issued by a bounded in-memory management session
table. The session boundary owns:

- immutable, non-reused session IDs;
- random opaque tokens tied to session IDs and principal generation;
- bounded session lifetime and monotonic expiry;
- explicit close and revocation on logout;
- invalidation of all active sessions when a principal is disabled or its
  credential changes;
- no persistent session tokens in configuration storage or secret state.

The existing `local_session` module proves the bounded session shape but is
not a production remote-session implementation. ITEM-320 owns the management
session publication and enforcement boundary.

## Request Authentication Semantics

An authenticated management request uses an exact `Authorization: Bearer`
request header with one bounded opaque token. Query parameters, cookies,
Basic authentication, and raw credential submission are not authenticated
request mechanisms.

The enforcement position is before protected management routes:

1. Real TLS 1.3 terminates and authenticates the transport peer only to the
   extent proved by the existing certificate boundaries.
2. The management authentication boundary validates the header and session.
3. ITEM-321 authorization validates role/capability claims before route
   dispatch.
4. The management route executes only after authentication and authorization
   succeed.

Unauthenticated management discovery may report that authentication is
available or unavailable without disclosing management state. Protected
system, network, and service data routes require an authenticated session
after Wave E publication. A missing, malformed, unknown, expired, disabled,
or throttled authentication attempt returns the stable unauthenticated
result (`401 Unauthorized`). An authenticated principal denied by the
authorization boundary returns `403 Forbidden`.

Request processing preserves bounded correlation IDs so audit records can
connect session, principal, route, and result without including secret
material.

## Failure Handling

Authentication failures fail closed:

- missing, invalid, expired, disabled, and throttled sessions produce one
  stable unauthenticated response category;
- failure responses do not disclose whether a principal ID exists, whether a
  credential was wrong, or which bounded failure cause applied;
- failed attempts are bounded per principal and do not block unrelated
  principals, TLS handshakes, read-only generic health routes, or local
  recovery;
- request authentication performs no allocation, storage I/O, audit write,
  or interrupt-context access;
- audit records contain result categories and bounded correlation IDs, never
  credential or session values;
- diagnostics redact all secret-bearing input and session material.

Credential loss recovery is a local, audited operation. It is not exposed as
a remote administrative discovery endpoint. Recovery behavior is finalized
by ITEM-320 and protected by ITEM-322.

## Secure Defaults

The Wave E implementation must keep these defaults:

- `auth=unavailable` until an administrative credential is initialized and
  the authentication boundary is implemented and proven;
- no default administrative password;
- no remote first-boot credential provisioning;
- TLS 1.3 as the only management transport;
- deny-by-default authorization after authentication;
- bounded sessions, bounded failure tracking, and bounded credential input;
- no raw secret, token, or credential value in logs, audit, HTTP output,
  support bundles, or release artifacts.

## Wave E Boundaries

Wave E ownership is fixed as:

- ITEM-319: this architecture and its host-guarded documentation contract;
- ITEM-320 owns administrative authentication implementation, credential
  initialization, session publication, and request enforcement;
- ITEM-321 owns management authorization implementation and route policy;
- ITEM-322 owns secret and credential protection implementation;
- controlled network/service mutation only after ITEM-321 completes Wave E.

The corrected execution wave remains authoritative in
`docs/hfoc-appliance-platform-architecture-audit.md`. No route changes in
this architecture item remove the current read-only or `auth=unavailable`
security boundary.

## Non-Goals

- No administrative authentication mechanism implementation.
- No credential initialization, reset, rotation, or persistence.
- No session creation, token generation, request enforcement, or HTTPS route
  change.
- No authorization or secret-protection implementation.
- No production normal-boot management activation.
- No new QEMU proof, because this item changes no runtime behavior.
- No release, version, or tag change.
