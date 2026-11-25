# HFOC Production Service / HTTPS Activation

**Date:** 2026-09-11
**Target:** ITEM-364 of the HFOC Appliance Platform production wave
**Composition:** `docs/hfoc-production-service-https-composition.md`
**Release contract:** HFOC-APPLIANCE-V1
**Status:** Implementation and software proof complete on the feature
branch; branch CI, Ready, merge, and post-merge gates are pending.

## Purpose

ITEM-353 defined the caller-owned `appliance_production_service_https`
composition boundary but deliberately did not activate it during normal Limine
boot. ITEM-364 adds the ordinary-boot activation: after the ITEM-360 storage
and state bootstrap, the ITEM-362A NIC/lwIP backend, and the ITEM-363 first
boot, ordinary boot now initializes the fixed HTTPS service boundary, drives
the ITEM-353 composition in the required order, and publishes one bounded
activation record.

The activation is honest. It never fabricates network, entropy, or certificate
readiness, and it never treats a QEMU fixture as physical evidence. The frozen
ordinary boot publishes an explicit awaiting state instead of a fictitious TLS
endpoint; the software proof exercises the running and restart paths with
explicit readiness evidence.

## Reusable Seams

- ITEM-353 supplies `appliance_production_service_https` with bounded states,
  evidence validation, link-loss fail-closed semantics, and a canonical
  snapshot.
- ITEM-312 supplies `appliance_https_service` with the fixed HTTPS service
  descriptor, lifecycle configuration, and the LISTENING/drain/restart
  boundary.
- ITEM-311 supplies `appliance_service_manager` for configured, ordered,
  stopped/running manager transitions.
- ITEM-313 supplies `appliance_persistent_service_config` evidence loaded by
  the ITEM-360 state loader.
- ITEM-352/362A/362B supply the production network lifecycle and the active
  IPv4 wire state used as the network readiness bit.
- ITEM-325 supplies the HTTPS certificate dependency vocabulary
  (`appliance_secret_persistence_https_readiness`).

## Implemented Boundary

Two always-compiled modules under `kernel/`:

- `appliance_production_service_activation` is caller-owned and
  allocation-free. It drives the always-compiled ITEM-353 composition in the
  required order, publishes `NONE`, `STOPPED`, `AWAITING_EVIDENCE`,
  `AWAITING_NETWORK`, `AWAITING_READINESS`, `RUNNING`, `DEGRADED`, and
  `FAILED`, and renders a bounded `production_service_activation` record with
  the fixed `secrets=NONE` marker. It never accepts, copies, or renders
  credentials, private keys, or certificate payloads.
- `appliance_production_service_boot` is the ordinary-boot glue compiled with
  `HFOC_PRODUCTION_SERVICES`. It owns the fixed V1 service state, the ITEM-353
  composition, and the activation object. It binds the persisted service
  evidence from the ITEM-360 state loader and the explicit
  network/entropy/certificate readiness supplied by ordinary boot.

The frozen baseline keeps the ITEM-311 service manager and the ITEM-312 HTTPS
lifecycle boundary host/QEMU-only, so the production kernel does not link
them. The V1 profile is exactly one fixed HTTPS service that is configured and
stopped by the kernel baseline; the activation binds that bounded state as its
service evidence. The real manager and HTTPS lifecycle remain exercised by
their existing host/QEMU proofs. The activation performs no route dispatch,
TLS handshake, storage I/O, or audit write.

## Readiness And Ordering

The activation accepts the composition readiness triple:

```text
network_active
entropy_ready
certificate_ready
```

and executes exactly this order once all three are present:

1. publish the composition readiness triple;
2. `appliance_production_service_https_start` (composition `STARTING`);
3. `appliance_production_service_https_running` (composition `RUNNING`).

A missing network bit leaves the record `AWAITING_NETWORK`. A missing entropy
or certificate bit leaves the record `AWAITING_READINESS`, and the composition
stays `CONFIGURED`. A later link loss calls
`appliance_production_service_https_fail_closed`; the record becomes
`DEGRADED` and the composition is unpublished.

A restart is a bounded stop, reset, reconfigure, and restart cycle through the
same composition contract: the activation stores the validated evidence,
drives `stop`/`stopped`/`reset`/`configure`, re-runs the readiness and start
sequence, increments the restart counter, and keeps the record `RUNNING`. The
real HTTPS lifecycle restart remains owned and proven by the host/QEMU
boundary.

## Ordinary-Boot Wiring

`kernel/src/kernel.c` initializes the activation after the ITEM-363 first-boot
block and polls it from the HLT event loop. The network bit comes from the
active ITEM-362B lease. The entropy and certificate bits are supplied as
`false` because the frozen baseline has no ordinary-boot owner for them yet;
the activation therefore publishes `AWAITING_NETWORK` or
`AWAITING_READINESS` honestly and never publishes a fictitious endpoint.

## Software Proof

| Gate | Target | Evidence |
| --- | --- | --- |
| Activation host | `make test-appliance-production-service-activation-host` | `Appliance production service activation host tests: ALL OK` |
| Ordinary-boot QEMU | `make test-production-service-https-qemu` | `[PRODUCTION-SERVICE] production_service_activation` record with honest readiness and `secrets=NONE` |
| Composition regression | `make test-appliance-production-service-https-host` | `Appliance production service HTTPS host tests: ALL OK` |
| Full regression | `make test` | PASS |

The host proof source is `tests/appliance-production-service-activation-host.c`.
It drives the real ITEM-353 composition with the activation contract. It
checks the awaiting-evidence, awaiting-network, awaiting-readiness, running,
restart, link-loss fail-closed, and stop paths, asserts the exact canonical
record, and asserts that the rendered record never contains secret material.
The real ITEM-311 manager and ITEM-312 HTTPS lifecycle remain covered by their
existing host and supervisor-composition QEMU proofs, which the production
kernel deliberately does not link.

The ordinary-boot QEMU proof source is
`scripts/test-qemu-production-service-https.sh`. It boots the plain production
ISO against one writable AHCI HFOC partition with one virtio-net device and
requires the `[PRODUCTION-SERVICE]` activation record with an honest awaiting
or degraded state and the `secrets=NONE` marker.

QEMU proof is software acceptance only. Physical service lifecycle, physical
TLS endpoint, physical certificate handling, and physical appliance runtime
remain `NOT_TESTED` until a separate Hardware Acceptance Report records actual
evidence.

## Limitations

- The activation accepts one appliance state, one network wire, and one fixed
  HTTPS service profile in V1.
- It does not provide DHCP/DNS (ITEM-362B), authentication/authorization
  (ITEM-365), the read-only management plane (ITEM-366), or mutating
  operations (ITEM-367A/367B).
- It does not implement a socket accept loop, a TLS handshake, certificate
  generation, storage I/O, or audit writes.
- It does not claim that QEMU composition proves physical appliance or
  production-browser compatibility.

## Non-Goals

- No DHCP/DNS wire change (ITEM-362B owns that path).
- No management route, web UI, authentication, or destructive action.
- No VERSION or release-number change and no v1.0.0.
- No physical-hardware acceptance.
