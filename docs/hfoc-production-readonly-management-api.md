# HFOC Production Read-Only Management API

**Date:** 2026-09-12
**Target:** ITEM-400 of the HFOC Appliance Platform productization wave
**Release contract:** HFOC-APPLIANCE-V1
**Status:** Implementation, host proof and QEMU route-resolution proof on the
feature branch; branch CI, Ready, merge and post-merge gates are pending.

## ITEM-505 update: the network target is no longer read-only

This document records what ITEM-400 published.  ITEM-505 removes the read-only
property from the network route alone, deliberately and with its own
two-phase contract:

- `GET /api/v1/network` and `GET /api/v1/network/status` keep the exact
  ITEM-400 bodies and remain behind the same request gate;
- `POST /api/v1/network` (and the panel form `POST /panel/network`) now reach
  the same `NETWORK_CONFIG` two-phase mutation the ITEM-367A plane already
  owns.  The operator stages a DHCP or static candidate, confirms the
  single-use token inside the bounded window, and the commit writes the
  durable `appliance_persistent_network_config` record through the same setter
  the console menu and the install declaration use;
- the commit does **not** rebind the live interface: it publishes
  `[PRODUCTION-NET-SET] source=PANEL … persisted=OK applies=reboot current=…`,
  and the next boot applies the saved configuration.  The panel Network page
  shows the active reading and the saved reading separately, so the two facts
  are never conflated;
- every other target this document describes stays read-only.

## ITEM-505 increment 3: the system hostname target is no longer read-only

The same rule now applies to the machine's own hostname, and to the POST of the
system target alone:

- `GET /api/v1/system` and `GET /api/v1/health` keep the exact ITEM-400 bodies
  and remain behind the same request gate;
- `POST /api/v1/system` (and the panel form `POST /panel/system`) reach a
  `SYSTEM_HOSTNAME` two-phase mutation of the ITEM-367A plane.  The operator
  stages one `hostname=…` field, confirms the single-use token inside the
  bounded window, and the commit writes the durable
  `appliance_persistent_config` record through the same
  `appliance_persistent_config_set_hostname()` setter the first-boot wizard
  uses;
- the commit does **not** rebind the running identity or the certificate it
  serves: it publishes `[PRODUCTION-SYSTEM-SET] source=PANEL hostname=…
  persisted=OK applies=reboot current=…`, and the next boot resolves the saved
  name as its identity.  The panel System page shows the active identity and
  the saved hostname separately, so the two facts are never conflated;
- every other target this document describes stays read-only.

The GET of `/api/v1/system` is unaffected: the mutation gate resolves the
hostname target explicitly and the read path never consults that branch.

## ITEM-506 increment 6: the account target

The one account has a mutation target of its own, and it is not a read route:

- `POST /api/v1/system/account` (and the panel form `POST /panel/account`)
  reach an `ACCOUNT_CREDENTIAL` two-phase mutation of the ITEM-367A plane.  The
  operator stages one four-field body -- `current`, `login`, `password`,
  `repeat` -- and confirms the single-use token inside the bounded window; the
  stage proves `current` with the enrollment owner's verify-only entry and
  validates the new pair with the credential model's policy, so nothing is
  written before the token comes back;
- the commit calls
  `appliance_production_admin_enrollment_provision_credential()`, the single
  setter: it draws a fresh salt, writes the PBKDF2 record, republishes the
  verifier through the request gate and closes every issued session, including
  the session that confirmed the change.  The old cookie and the old bearer
  answer `401` from the next request on;
- the commit publishes `[PRODUCTION-CREDENTIAL-SET] source=PANEL
  login=<login> changed=LOGIN,PASSWORD sessions=REVOKED persisted=OK` (with
  `changed=PASSWORD` when the login did not move) and never the password or a
  hash of it;
- `GET` of the target has no read meaning: the API target resolves to no read
  route and is answered with the frozen unknown-route refusal (`404`) before
  the request gate, and the panel target `GET /panel/account` is the rendered
  Account page;
- every other target this document describes stays read-only.

## Purpose

Before this item an ordinary production boot resolved exactly three public
targets: the rendered root document, `/health` and the `/api/v1` discovery
body, plus the frozen method and unknown-route refusals.  The four read-only
management targets -- `/api/v1/system`, `/api/v1/network`,
`/api/v1/network/status`, `/api/v1/service` and `/api/v1/health` -- were served
only by the macro-gated QEMU management fixtures, which compile their own
snapshot owners.  The production kernel linked the four API modules but never
published their snapshots, so a real appliance answered every one of those
targets with the frozen unknown-route body.

ITEM-400 closes that gap:

- `kernel/src/appliance_production_management_api.c` is the ordinary-boot
  read-only snapshot owner.  It is caller-owned and allocation-free, consumes
  the live coverage of the ITEM-360 boot coordinator, the ITEM-349 state
  loader, the ITEM-362 network wire and the ITEM-364 service/HTTPS activation,
  and renders the four targets through the existing ITEM-315..318/326 API
  modules;
- `appliance_production_serving_publish_management_api()` binds that owner to
  the serving path.  A target the owner resolves is known before the ITEM-365
  request gate is consulted, so it is answered by the gate (401/403/503) and
  never by the unknown-route body; the discovery target stays with the
  management API module and every other target keeps the frozen behaviour;
- ordinary boot assembles the evidence once after the serving path is
  initialized and republishes it from the event loop, so a link, lease,
  certificate or listener change is visible to the next request.

## Honest evidence

The owner publishes what the live owners actually report:

| Target | Evidence |
| --- | --- |
| `/api/v1/system` | ITEM-302 identity, boot readiness/mode, state-loader configuration source and hostname, uptime, fixed V1 process count 0 |
| `/api/v1/network` | ITEM-349 committed/default network configuration and its real source |
| `/api/v1/network/status` | ITEM-310 diagnostics published from the live link, the active IPv4 configuration, the DHCP lease and a resolved DNS answer |
| `/api/v1/service` | ITEM-364 activation state translated into the existing manager snapshot vocabulary |
| `/api/v1/health` | ITEM-326 aggregate over boot, storage, configuration, network, service and HTTPS-management verdicts |

A missing owner is rendered `UNAVAILABLE`, an awaiting or degraded one
`DEGRADED`/`WARN`, and a failure `FAILED`; nothing is rendered as a PASS that
the evidence did not produce.  The reachability fields are derived from the
wire's own lease and answer -- they are not an active probe, and an unknown
reachability is published as `UNKNOWN`, never as reachable.

## What is proven where

| Claim | Proof |
| --- | --- |
| Snapshot bodies, honest degraded/unavailable rendering, route ownership, method refusal and allocation-free operation | `make test-appliance-production-management-api-host` (`Appliance production management API host tests: ALL OK`), 40 assertions |
| The four targets resolve on the ordinary production image and are answered by the request gate instead of the unknown-route body | `make test-production-service-https-qemu` (`TLS-PROOF read-only management targets: known routes, gate denial 503`) |
| The production kernel still links and boots | `make iso`, `make build/hfoc-kernel.elf` |

## Limitations

- **No administrator credential is enrolled on ordinary boot, so the request
  gate answers every protected target with `503`.**  The snapshot bodies are
  proven by the host proof, but the authenticated `200` path needs the
  credential-enrollment and local-session-issuance owner, which is queued as
  ITEM-401.  This item deliberately does not fabricate a credential and
  does not weaken the gate to make the routes answer `200`.
- **The `/api/v1/network/status` reachability evidence is passive** (a DHCP
  lease and a resolved DNS answer).  No ICMP/gateway probe is issued from the
  serving path.
- **The service snapshot is a translation of the fixed V1 HTTPS activation**,
  not an ITEM-311 manager instance: the frozen ordinary-boot baseline does not
  link the service manager, exactly as ITEM-364 documents.

## Contracts

No test, verifier or `check.sh` contract was weakened.  The new host proof
joins the canonical host suite; the manifest regression
(`tests/test-host-suite-manifest.sh`) and the runner derive the suite size from
the same serial recipe instead of a hardcoded count, so the added target cannot
desync the manifest.
