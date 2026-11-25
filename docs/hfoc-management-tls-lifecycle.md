# HFOC Management TLS Lifecycle and Key Rotation

**Date:** 2026-09-11
**Target:** ITEM-375 of the HFOC Appliance Platform productization wave
**Dependencies:** ITEM-364 (service/HTTPS activation), ITEM-374 (network modes,
time synchronization, management ingress binding)
**Release contract:** HFOC-APPLIANCE-V1
**Status:** Implementation and software proof complete on the feature branch;
branch CI, Ready, merge, and post-merge gates are pending.

## Purpose

ITEM-364 activated the fixed HTTPS service on ordinary boot but had no
ordinary-boot certificate owner: the readiness triple was supplied as
`network_active`, `entropy_ready=false`, `certificate_ready=false`, so the
activation honestly stayed `AWAITING_*` and never raised a TLS endpoint.
ITEM-374 added the validated wall clock and the fail-closed management ingress
binding but left the listener binding to this ITEM.

ITEM-375 closes the certificate half of that gap:

- the management TLS policy is a decision, not a comment — every legacy
  protocol, weak or non-AEAD cipher, static key exchange, weak signature,
  compression and renegotiation is refused with an exact reason;
- the certificate lifecycle is a bounded request/import contract: a
  certificate must answer the pending request, satisfy the policy and be
  inside its validity window on the validated wall clock before it may serve;
- rotation is staged, so the serving certificate never stops being ready
  while a replacement is validated, and an unpromotable replacement is
  dropped instead of left dangling;
- ordinary boot owns the certificate readiness bit from the lifecycle instead
  of a literal, declares the certificate the persisted hostname needs, and
  publishes one bounded honest record;
- the management listener publishes HSTS on every response it serves.

What this ITEM deliberately does not do: it does not terminate TLS itself
(that is the frozen mbedTLS boundary), it does not store a certificate or a
private key (the DER bytes stay with the caller and the manifest boundary), it
does not implement the ITEM-385 reachability policy (client certificates,
source allowlists, remote-session audit, break-glass), and it does not
implement the ITEM-384 browser hardening surface (CSP, rendered sections).
`VERSION` is not changed.

## The compiled-in test certificate has a life, and it is checked

Every HTTPS QEMU proof curls the appliance with the certificate compiled into
`kernel/src/hfoc_mbedtls_test_secret.c` (`CN=hfoc.test`, EC P-256, self-signed
CA), and curl verifies that certificate against the wall clock.  The first
certificate had a **30-day** window (2026-08-29 to 2026-09-28 07:42 UTC) and
expired mid-flight: from that instant every PR whose acceptance phase runs those
proofs failed with `curl: (60) SSL certificate problem: certificate has expired`
buried in a guest log, while the static-host phase stayed green and a main push
run (which does not run the heavy phase) was still green - the failure looked
like a flake and was a clock.

ITEM-493 renews it to a **20-year** window (2026-09-28 to 2046-09-23, sha256
`1552a388…`) and, more importantly, makes the class impossible to repeat
silently:

* `tests/test-mbedtls-secret-validity.sh` extracts the same DER bytes the kernel
  carries, requires the subject to be the `hfoc.test` certificate, requires
  `openssl x509 -checkend 31536000` to pass (one year,
  `HFOC_TEST_SECRET_MIN_SECONDS` overrides it), and requires the bytes to match
  the digests `kernel/include/hfoc_mbedtls_test_secret.h` declares;
* the check is registered in the canonical host suite
  (`test-mbedtls-secret-validity-host`), so it runs in the `static-host` phase of
  every PR, where the cause is one line instead of a guest transcript;
* the contracted slice `scripts/check.d/05-certificate-key-fail-closed.sh` pins
  the check, its one-year floor and its registration.

Renewing it again is one command pair and one digest update, in one commit:

```text
openssl ecparam -name prime256v1 -genkey -noout -out key.pem
openssl req -x509 -new -key key.pem -sha256 -days 7300 -subj "/CN=hfoc.test" \
    -addext "basicConstraints=critical,CA:TRUE" \
    -addext "subjectKeyIdentifier=hash" \
    -addext "authorityKeyIdentifier=keyid:always" -out cert.pem
```

then `openssl x509 -outform DER` / `openssl ec -outform DER` into the two arrays
of `hfoc_mbedtls_test_secret.c`, and the two SHA-256 digests in the header and in
the `Makefile`'s test-secret manifest expectation.

## Reusable Seams

- ITEM-297 supplies `hfoc_mbedtls_manifest_parse`, which already proves the
  certificate chain, the EC P-256 key, the `hfoc.test`-style hostname SAN, the
  certificate digest and the certificate/private-key match from DER bytes.
  This module owns the lifecycle *above* that parser and never re-parses DER.
- ITEM-364 supplies `appliance_production_service_activation` with the
  `network_active` / `entropy_ready` / `certificate_ready` readiness triple and
  the `AWAITING_*`/`RUNNING`/`DEGRADED` record.
- ITEM-374 supplies `appliance_management_bind` (`LOCAL_ONLY` /
  `MANAGEMENT_INTERFACE` / `OUTER_INTERFACE`, peer allowlist) and
  `appliance_production_network_time_epoch_seconds`, the monotonic-anchored
  wall clock that is zero until an SNTP sample is validated.
- ITEM-262 freezes the mbedTLS profile to server TLS 1.3 with
  `TLS_AES_128_GCM_SHA256` and P-256 ECDHE/ECDSA; this module publishes the
  matching policy so the profile and the policy cannot drift apart silently.
- ITEM-277 supplies the deterministic `http_response` fixed-header encoder
  that the management listener publishes.

## Implemented Boundary

`appliance_management_tls` is an always-compiled, allocation-free,
metadata-only module under `kernel/`:

- **Policy.** `appliance_management_tls_policy_check` accepts exactly one
  profile (TLS 1.3, `TLS_AES_128_GCM_SHA256`, ephemeral P-256 ECDHE, ECDSA
  P-256 SHA-256, no compression, no renegotiation) and returns
  `UNKNOWN_PROTOCOL`, `WEAK_PROTOCOL`, `UNKNOWN_CIPHER`, `WEAK_CIPHER`,
  `UNKNOWN_KEY_EXCHANGE`, `WEAK_KEY_EXCHANGE`, `UNKNOWN_SIGNATURE`,
  `WEAK_SIGNATURE`, `COMPRESSION_ENABLED` or `RENEGOTIATION_ENABLED`
  otherwise. `appliance_management_tls_policy_frozen` publishes the accepted
  profile.
- **Request.** `appliance_management_tls_request_certificate` derives a
  bounded request (request id, common name, subject alternative name, policy
  generation, creation time) from the appliance identity. An empty or
  malformed identity is refused; a second request while one is pending is
  refused.
- **Import.** `appliance_management_tls_import_certificate` validates the
  certificate metadata against the pending request, the policy and the wall
  clock, and refuses with `NO_PENDING_REQUEST`, `BAD_CERTIFICATE`,
  `WEAK_CERTIFICATE_KEY`, `CERTIFICATE_MISMATCH`,
  `CERTIFICATE_NOT_YET_VALID`, `CERTIFICATE_EXPIRED` or `SAME_CERTIFICATE`.
  An unsynchronized clock (epoch 0) is refused with `BAD_CLOCK` instead of
  validating a certificate against a fabricated time. The module never
  receives a private key or a certificate payload.
- **Rotation without downtime.** A first accepted import becomes the active
  certificate. A later accepted import is *staged*: the active certificate
  keeps serving and `certificate_ready` stays true. Only an explicit
  `appliance_management_tls_promote` swaps the slot, and it re-validates the
  staged certificate at promotion time. A staged certificate that is not yet
  valid stays staged (it may still be promoted once its window opens); an
  expired or otherwise unpromotable one is dropped and the listener returns to
  serving the active certificate. `appliance_management_tls_discard_staged`
  abandons a rotation without touching the active slot.
- **Ingress.** Every request/import/promotion carries an explicit ingress
  (`LOCAL` or `NETWORK`). A local console operation always works; a network
  operation is admitted only when the ITEM-374 binding admits it, so the
  default `LOCAL_ONLY` binding denies a network certificate operation with
  `DENIED_OUTER`, an unknown peer is denied with `DENIED_PEER`, and an
  unpublished binding denies with `DENIED_UNCONFIGURED`.
- **Readiness.** `appliance_management_tls_readiness` publishes
  `certificate_ready` only while an active certificate exists, is
  policy-compliant and is inside its validity window at the supplied epoch.
  An unsynchronized clock (epoch 0) never yields readiness.
- **Tamper resistance.** Every operation re-checks the record: an unknown
  version is refused with `BAD_VERSION` and a policy field that no longer
  matches the frozen profile is refused with `BAD_POLICY`, so a corrupted
  in-memory record cannot silently downgrade the listener.
- **Record.** `appliance_management_tls_render` publishes one bounded line
  with the state, the frozen protocol and cipher, honest certificate
  readiness, staging and counter evidence, the HSTS lifetime and the fixed
  `secrets=NONE` marker. It never renders an identity, a private key or a
  certificate payload.

## HSTS

The management listener terminates TLS, so the shared fixed response head now
carries `Strict-Transport-Security: max-age=31536000`. Because it is part of
the fixed head it is published on error responses too, and no handler can
forget it. `APPLIANCE_MANAGEMENT_TLS_HSTS` and
`APPLIANCE_MANAGEMENT_TLS_HSTS_MAX_AGE` are the single source of the value.

## Ordinary-Boot Wiring

`appliance_production_service_boot` now owns the management TLS lifecycle next
to the ITEM-353 composition and the ITEM-364 activation:

1. `appliance_production_service_boot_init` initializes the lifecycle and, when
   the ITEM-360 state loader published a hostname, declares the certificate the
   appliance needs with a local request for that hostname.
2. `appliance_production_service_boot_poll_clock` takes the network bit, the
   entropy bit, the clock verdict, the monotonic tick and the wall-clock epoch
   — but no certificate bit. The certificate bit is derived from
   `appliance_management_tls_readiness`, and only while the clock is
   synchronized.
3. `kernel.c` prints the lifecycle record as
   `[PRODUCTION-TLS] management_tls v=1 ...` whenever the state changes.

Ordinary boot owns no certificate and no private key, so an unprovisioned
appliance publishes exactly:

```text
[PRODUCTION-TLS] management_tls v=1 state=AWAITING_CERTIFICATE protocol=TLS13 cipher=AES_128_GCM_SHA256 certificate=UNAVAILABLE staged=NONE request=1 import=0 promote=0 rotate=0 reject=0 hsts=max-age=31536000 secrets=NONE
```

The activation therefore still reports `certificate=UNAVAILABLE` and stays
`AWAITING_READINESS`, which is the honest state rather than a fabricated TLS
endpoint.  ITEM-386 supplies both missing owners: an ordinary-boot entropy
owner (hardware RDSEED/RDRAND through the accumulator/seed/CTR-DRBG chain) and
a per-appliance self-signed certificate generated at first boot.  The lifecycle
record only leaves `AWAITING_CERTIFICATE` once a validated wall clock exists,
because `import` and `promote` refuse an unvalidated clock by contract; the
listener itself does not wait for that and serves the same certificate.

ITEM-404 completes that wiring.  Four defects kept the lifecycle at
`AWAITING_CERTIFICATE` on every ordinary boot, each masked by the one before
it:

1. the serving path took the wall-clock epoch once at initialization, before
   the ITEM-374 SNTP sample could ever land, and nothing refreshed it, so
   `import`/`promote` always saw epoch zero;
2. the lifecycle was only ever asked for a certificate when the ITEM-360 state
   loader published a persisted hostname, while the certificate owner serves
   one from the documented default identity when the volume is absent or
   empty — on a diskless boot the record stayed `UNCONFIGURED` (`request=0`)
   while a certificate was being served;
3. the certificate structure handed to the lifecycle carried no validity
   window and no fingerprint, so even with a validated clock the import would
   have been refused as a malformed certificate;
4. a successful import on an empty lifecycle activates the certificate
   directly, but the path then called `promote` for a rotation that did not
   exist, turning the first certificate into a rejected `NOT_STAGED` call.

ITEM-404 fixes all four:

- `appliance_production_serving_poll` now takes the live wall-clock epoch from
  `appliance_production_network_time_epoch_seconds` on every event-loop turn,
  consumes it once it is validated and never lets an unvalidated (zero)
  reading erase a validated one;
- `appliance_production_service_boot` declares the certificate request with
  the identity the certificate owner will actually use: the persisted
  hostname when it is readable and bounds-valid, the documented default
  `hfoc-appliance` otherwise, so the request is never `UNCONFIGURED` on a boot
  whose volume is absent;
- `appliance_production_certificate` parses its own DER back into the
  `not_before`/`not_after` window and publishes the SHA-256 fingerprint of the
  served bytes, and a stored record whose DER cannot be parsed is treated as
  corrupt and regenerated instead of served;
- the serving path imports the boot certificate directly into the active slot
  and treats readiness under the validated clock as the terminal state; a
  promotion stays the rotation owner's operation.

The promoted ordinary-boot record is therefore:

```text
[PRODUCTION-TLS] management_tls v=1 state=ACTIVE protocol=TLS13 cipher=AES_128_GCM_SHA256 certificate=READY staged=NONE request=1 import=1 promote=0 rotate=0 reject=0 hsts=max-age=31536000 secrets=NONE
```

The listener is up before that happens and keeps serving the same certificate
throughout; the record is re-emitted by `kernel.c` when the state changes.

## Software Proof

| Gate | Target | Evidence |
| --- | --- | --- |
| Lifecycle host | `make test-appliance-management-tls-host` | `Appliance management TLS host tests: ALL OK (116 checks)` |
| Ordinary-boot QEMU | `make test-production-service-https-qemu` | `[PRODUCTION-TLS]` record with `state=AWAITING_CERTIFICATE`, the frozen policy, HSTS and `secrets=NONE` |
| Validated-clock QEMU | `make test-production-dhcp-dns-qemu` | the SNTP-validated boot publishes `state=ACTIVE` with `certificate=READY`, `import=1` and `reject=0` after `[PRODUCTION-NET-MODE] mode=DHCP clock=READY` |
| Response-header regression | `make test-http-response-host`, `make test-https-handler-host`, `make test-http-acceptance-host` | `ALL OK` with the HSTS header in the fixed head |
| Activation regression | `make test-appliance-production-service-activation-host` | `Appliance production service activation host tests: ALL OK` |
| Full regression | `make test` | PASS |

The host proof `tests/appliance-management-tls-host.c` drives the real policy
table, the request/import/refusal matrix, activation and expiry, staged
rotation, refused and discarded promotions, the ingress gate against the real
ITEM-374 binding, the canonical record and the HSTS header, and it asserts
that the rendered record never contains identity or secret material.

## Operator certificate import (ITEM-503)

The ordinary-boot certificate is the appliance's own; an operator who wants the
browser to stop warning can now upload their own certificate through the
appliance instead of trusting a certificate the appliance generated for
itself. The path is:

```
POST /panel/security            (the Security panel form)
POST /api/v1/system/certificate (the scriptable target)
        |
        v
mutation CERTIFICATE_IMPORT, command 12 (admin + configuration-write)
        |
        v
stage: form -> PEM -> appliance_certificate_import_validate()
       (pair, EC P-256, window vs validated clock, SAN vs declared name,
        every chain member)
        |
        v
commit: exact single-use token -> certificate record v2 in the
        CERTIFICATE store: magic, version 2, provenance, leaf, chain, key
        (12 blocks; a v1 record keeps being read)
        |
        v
next boot: the serving path loads the record and appends the chain to the
        mbedTLS certificate list, so the handshake sends leaf + chain
```

The import is deliberately two-phase and writes nothing before the exact
token, the validation runs again at commit time (the clock may have moved), and
a cancelled or expired candidate leaves the stored record untouched. The
listener's TLS configuration is built once per boot, so `[PRODUCTION-SERVING]`
publishes `cert_imports=<n> cert_applies=reboot`: the import is durable
immediately and is served after the next reboot, and the record never claims
otherwise. The issuer name in the `[PRODUCTION-TLS]` lifecycle record comes
from the served DER, so an operator certificate is not described as
self-signed.  The owner authorised the profile changes on 2026-10-03;
ITEM-508 increment 1 defined `MBEDTLS_SSL_CLI_C` (profile version 3) for the
ACME module - the only caller the item allows - while RSA and PEM stay
disabled; the ACME flow itself is the rest of ITEM-508.  ITEM-507 increment 1 defined
`MBEDTLS_SSL_SERVER_NAME_INDICATION` (profile version 2), so a certificate can
be selected by exact name at handshake time.  SNI selects a presentation, never
trust.  ITEM-507 increment 6 proves the selection on a live listener with two
declared names: the operator import refuses a pair while the wall clock is
unvalidated (`BAD_CLOCK`), so the proof boots a fixture kernel that writes two
test-only pairs through the same durable writer a panel commit uses, and then
requires two different SNI names to receive two different certificates, an
undeclared name to be refused and a client without SNI to receive the
appliance's own identity.

## Honest Limits

- The certificate is now imported on ordinary boot by ITEM-386.  When no
  operator certificate has been provisioned the appliance generates one
  self-signed P-256 certificate at first boot, persists it in the reserved
  CERTIFICATE store of the state volume and serves it.  A self-signed
  certificate produces a browser warning, it is per-appliance, and it is
  replaced by an operator certificate through the ITEM-325 request/import/
  promote path.  The private key is never rendered, never logged and never
  copied outside the store; when the volume is sealed the plaintext write is
  refused and the appliance serves a per-boot certificate with `DEGRADED`
  published instead.
- An ordinary-boot entropy owner now exists (ITEM-386).  Since ITEM-404 the
  lifecycle reaches `ACTIVE` on an ordinary boot whose clock has been validated
  by an SNTP sample, and the request is declared with the identity the
  certificate owner serves even when the state volume is absent.  The frozen
  baseline has no RTC, so a boot with no network time still publishes
  `AWAITING_CERTIFICATE` with `request=1` and serves the certificate; the
  lifecycle record is honest about the unvalidated clock instead of pretending
  the certificate was validated.
- Operator-driven rotation still has no ordinary-boot caller: the ITEM-325
  request/import/promote path and its staged rotation are host-proven, and the
  first boot certificate is imported active by the serving path, so a rotation
  staged by a future management operation remains its own promotion.  Physical
  certificate rotation is `NOT_TESTED`.
- The serving path completes a real TLS 1.3 handshake (ITEM-386).  The ordinary
  production boot raises a live TLS 1.3 listener, publishes the hardware entropy
  source and the appliance certificate, replenishes the production receive ring
  after every completion and recycles the transmit ring, so the listener reads
  the client's complete first flight instead of one segment.
  `appliance_production_serving.c` then terminates TLS 1.3 and serves the frozen
  management surface: the root HTML with the eleven sections and the hardening
  headers, `/health`, the `/api/v1` discovery body, the `405` method refusal,
  the `404` unknown-route refusal and the panel sign-in refusal are all measured
  over TLS 1.3 by `scripts/test-qemu-production-service-https.sh`, whose client
  phase is fatal.  The two mbedTLS profiles in the image own disjoint, complete
  object partitions, verified by `scripts/verify-mbedtls-object-partition.sh`,
  and every diagnostic is published in the `[PRODUCTION-SERVING]` record.
- The listener binding is consumed here for the certificate operations; the
  full reachability policy (client certificates, source allowlist, remote
  session audit, break-glass) is ITEM-385.
- Physical TLS termination against a real client, a real certificate authority
  and a real HSTS-remembering browser is not exercised; the QEMU proof is a
  software proof, not physical evidence.
