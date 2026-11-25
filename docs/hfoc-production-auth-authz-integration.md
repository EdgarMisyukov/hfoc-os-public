# HFOC Production Auth/Authz Request Integration

**Date:** 2026-09-11
**Target:** ITEM-365 of the HFOC Appliance production integration queue
**Queue:** `docs/superpowers/plans/2026-09-11-production-integration-queue-362a-369.md`
**Depends on:** ITEM-320/ITEM-321 administrative authentication and management
authorization, ITEM-363 first-boot persistence, ITEM-364 production
service/HTTPS activation.
**Status:** Implementation and software proof on the feature branch; branch CI,
Ready, merge, and post-merge gates remain the canonical evidence.

## Purpose

The production management plane needs one request gate that binds the existing
administrative authentication boundary to the existing management authorization
matrix. Before ITEM-365 the read-only management composition rendered
`authorization=UNAVAILABLE` because no production caller composed the two
boundaries: authentication and authorization existed and were proven in the
macro-gated QEMU fixture, but no runtime path published availability.

ITEM-365 adds that caller. It does not create a new device registry, route
table, session store, or authorization policy: it composes
`appliance_admin_auth` and `appliance_management_authorization`, both already
merged.

## Implemented Boundary

The implemented module is `appliance_production_auth` under `kernel/`:

- caller-owned and allocation-free;
- publishes `NONE`, `UNINITIALIZED`, `PREPARED`, `AWAITING_CREDENTIAL`,
  `ACTIVE`, `STOPPED`, and `FAILED`;
- accepts one administrator credential verifier through
  `appliance_production_auth_publish_credential()` and never stores a plaintext
  secret;
- treats a live credential as rotatable only through the reset path, which
  revokes every open session before a replacement verifier is accepted;
- exposes `appliance_production_auth_authorize_request()`, which:
  - fails closed with `UNAVAILABLE` while no credential is active;
  - allows the public `GET /api/v1` discovery target anonymously;
  - denies an anonymous request to every other management target
    (`GET/HEAD /api/v1/system`, `/api/v1/network`, `/api/v1/network/status`,
    `/api/v1/service`);
  - allows an authenticated administrator session holding the admin role and
    the status-read capability;
  - denies unknown targets, unsupported methods, and non-management targets by
    default;
- renders one bounded record, `production_auth v=1 state=... auth=AVAILABLE`
  or `auth=UNAVAILABLE`, with allow/deny/anonymous/unavailable counters.

The module owns no storage I/O, network activation, HTTP dispatch, TLS, or
service lifecycle. It never fabricates availability: `auth=AVAILABLE` is
published only after a non-zero verifier is published and enabled.

## Production Management Integration

The ITEM-354 production management composition accepts
`authorization_available` as caller evidence. The ITEM-354 QEMU proof now
builds the `appliance_production_auth` composition, activates one administrator
session, checks one authorized request, and publishes the resulting
`authorization_available` value, so the canonical snapshot renders

```
production_management v=1 state=ACTIVE identity=PRESENT config=COMMITTED
network=ACTIVE services=ACTIVE https=READY authorization=AVAILABLE health=PASS
```

The previous `authorization=UNAVAILABLE` expectation is gone. The production
management module stays a pure renderer and does not embed the auth
composition.

## Software Proof

| Gate | Target | Evidence |
| --- | --- | --- |
| Auth host | `make test-appliance-production-auth-host` | `Appliance production auth host tests: ALL OK` |
| Management QEMU | `make test-appliance-production-management-qemu` | `authorization=AVAILABLE` in the supervisor-composition production management gate |
| Full regression | `make test` | PASS |

The host proof source is `tests/appliance-production-auth-host.c`. It covers
the unavailable fail-closed path, anonymous denial, authorized allow, public
discovery, unknown-route/method/non-management default denial, stop, reset,
credential rotation, and the exact `auth=AVAILABLE` render.

QEMU proof is software acceptance only. Physical management-plane and physical
authentication remain `NOT_TESTED`.

## Explicit Non-Goals

- No DHCP/DNS change (ITEM-362B) and no service/HTTPS change (ITEM-364).
- No mutation operations (ITEM-367A), update operations (ITEM-367B), or
  recovery executor (ITEM-368).
- No VERSION change and no physical acceptance.
