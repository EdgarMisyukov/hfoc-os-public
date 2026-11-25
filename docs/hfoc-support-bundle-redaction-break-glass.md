# HFOC Support Bundle, Redaction and Break-Glass

**Item:** ITEM-378 of the HFOC Appliance Platform productization wave
**Depends on:** ITEM-327 (runtime diagnostics and logs, `docs/hfoc-appliance-runtime-diagnostics-logs.md`),
ITEM-366 (read-only management plane, `docs/hfoc-appliance-read-only-management-plane.md`)
**Release contract:** HFOC-APPLIANCE-V1
**Status:** implementation and focused host proof; branch CI, Ready, merge and
post-merge gates pending.

## Purpose

A customer sends a support bundle when something is wrong, and an operator
asks for a remote session when the bundle is not enough. Both are, correctly,
the most feared capabilities of an appliance: the bundle can export state, and
the session can reach into a running system. ITEM-378 makes both of them
bounded, consented and auditable, and gives the operator a way back in when the
management plane itself is unreachable.

The item adds four boundaries on top of the existing ITEM-167/168/169 support
bundle:

| Boundary | Module | What it guarantees |
| --- | --- | --- |
| Signed diagnostic bundle | `appliance_support_bundle` | the exported bundle carries a canonical binding, a redaction attestation and a keyed authenticity seal; tampering, a leaked secret and a missing key are refused or published honestly |
| Consent-based remote session | `appliance_support_session` | a network request is inert until a locally present administrator consents; the grant is time-bounded, revocable, audited and has no shell scope |
| Break-glass recovery | `appliance_support_break_glass` | a local-presence-only, two-phase, single-use procedure with a fixed action set that restores `LOCAL_ONLY` and revokes remote sessions; it is not a shell and needs none |
| Production support plane | `appliance_production_support` | the three boundaries composed into one state the management plane publishes, with the break-glass effects performed against the real ingress policy and session |

## Signed diagnostic bundle

The ITEM-168 exporter already produces a manifest and a terminal SHA-256
digest, but anyone who can produce the bytes can produce the digest, and the
manifest never states whether secret configuration was withheld. A support
engineer therefore cannot tell a genuine, redaction-checked bundle from a
hand-assembled one.

`appliance_support_bundle` closes both gaps without changing the exporter
format:

1. **Redaction attestation.** The ITEM-169 diagnostic-privacy deny list
   (`"credential"`, `"secret"`, `"password"`, `"token"`, `"private_key"`,
   `"capability"`) is re-applied to the live configuration source with exactly
   the counting rule the manifest applies: only `CONFIG`-kind entries take
   part, secret-classified keys are *withheld* (they never reach the payload or
   the payload digest), everything else is *included*. The report records the
   policy id, the declared deny-list size, the entry counts and a `leaked`
   counter, and it is part of the signed binding.
2. **Canonical binding.** The module re-derives the exporter magic, schema,
   versions, entry count, exact bundle length, payload length, the embedded
   manifest digest and the terminal digest from the actual bytes, requires the
   redaction report to agree with the manifest's own `config_included` /
   `config_excluded` claim, and encodes all of it into one fixed-width
   160-byte little-endian string (`HFOC-SB-BIND-1`).
3. **Keyed authenticity seal.** The binding is sealed with HMAC-SHA-256
   through a caller-bound MAC boundary (`appliance_support_bundle_mac_fn`); the
   production composition binds `appliance_state_crypto_hmac_sha256` from the
   ITEM-373 state-crypto profile. Without a bound key the bundle is published
   as `UNSIGNED` with the exact reason (`NO_SEAL_KEY`) and a zeroed MAC — a
   seal is never fabricated.
4. **Verification.** `appliance_support_bundle_verify` re-derives the binding
   from the bytes, re-checks the redaction claim, re-computes the MAC and
   compares it in constant time. A rewritten payload byte, a rewritten
   manifest, a rewritten terminal digest, a rewritten redaction report, a
   foreign key and a truncated key are all refused.

Failing closed is explicit: a bundle with a leaked secret (`REDACTION_FAILED`)
produces no digest, no MAC and no claim; a missing MAC provider reports
`MAC_UNAVAILABLE`; a missing key reports `UNSIGNED`.

### What "signed" means here, exactly

The frozen baseline has no on-device asymmetric signing identity: an ECDSA
private key is not in the repository and never is, and the verification-only
Mbed TLS profile has no RNG or DRBG, so the appliance cannot produce an
asymmetric signature. The seal is therefore a **keyed HMAC-SHA-256
authenticity tag over the canonical binding**, not an ECDSA signature. Anyone
who holds the seal key (the appliance's state key material, or the escrowed
recovery key that unwraps it) can verify the bundle; a consumer without the key
can check the binding self-consistency but not authenticity, and the module
reports exactly that instead of a pass. Asymmetric signing of exported
artifacts belongs to the release signing ceremony (ITEM-380).

## Consent-based remote support session

`appliance_support_session` implements the one management capability a
customer is right to be afraid of:

- a session starts as a `REQUEST` from the network and is **inert** until a
  locally present administrator consents on the local console; consent that
  arrives over the network is refused (`REMOTE_CONSENT_REFUSED`) and a consent
  without an identified principal is refused (`DENIED`);
- the scope vocabulary is closed: `READ_DIAGNOSTICS` and `EXPORT_BUNDLE`, and
  nothing else. A scope bit outside that mask is refused, and
  `appliance_support_session_shell_permitted()` is a declared `false` with
  `APPLIANCE_SUPPORT_SESSION_SCOPE_SHELL == 0` — "no shell required" is a
  constant, not a promise;
- a granted session expires on the monotonic tick it was granted for
  (default 600 ticks, hard maximum 3600), can be revoked at any time by the
  local console or by an identified principal, and losing the composed
  dependencies terminates it (`UNAVAILABLE` never reports active);
- every transition — request, consent, denial, expiry, revocation, loss of
  availability — produces a bounded audit entry (tick, request id, event,
  outcome, requester, principal, scopes, origin) that the composition feeds to
  the ITEM-327 ring; the retained window is eight entries and dropped entries
  are counted, not hidden.

## Break-glass recovery, no shell required

Break-glass is what a locally present administrator does when the management
plane is unreachable or the administrative credential is lost. It is
deliberately not a shell and not a command channel:

- the action set is a fixed, declared list:
  `RESTORE_MANAGEMENT_LOCAL_ONLY` (force the ITEM-374 ingress policy back to
  `LOCAL_ONLY`) and `REVOKE_REMOTE_SUPPORT` (terminate any live support
  session). There is no path, no command id, no free-form input and no
  scripting runtime; `APPLIANCE_SUPPORT_BREAK_GLASS_ACTION_SHELL == 0` and
  `appliance_support_break_glass_shell_required()` is `false`;
- it is local-presence only: an arm or apply attempt that arrives over the
  network is refused before any effect (`REMOTE_REFUSED`);
- it is two-phase: arming publishes a single-use token bounded by
  `APPLIANCE_SUPPORT_BREAK_GLASS_CONFIRM_TICKS` (300 ticks); applying requires
  the exact token inside the window exactly once, so a replay, a wrong token
  and a stale window all refuse; the token is cleared on cancel, expiry,
  application and loss of availability;
- every transition is audited with the token, principal and action mask, and
  the applied mask is published so the operator can see exactly what was done.

The documented procedure is the local console itself (see
[Operating procedure](#operating-procedure)); it needs a keyboard and a
display, not a shell.

### Operating procedure

1. On the appliance console, request a session as usual; the local
   administrator sees the pending request and consents. A session that was not
   requested locally cannot be created, and a request that was not consented
   locally stays inert.
2. If the management plane is unreachable, arm break-glass locally with the
   declared action set, confirm the published token inside the 300-tick window,
   and the plane restores `LOCAL_ONLY` ingress and revokes any remote session.
3. Export the support bundle from the local console, hand the customer the
   bundle together with the seal key material (or the escrowed recovery key),
   and verify the seal before trusting the contents.
4. Every step above leaves an audit entry in the ITEM-377 chain; export the
   chain with the bundle so the customer receives evidence, not a claim.

## Production support plane

`appliance_production_support` composes the three boundaries with the ITEM-374
`appliance_management_bind` owner. The plane publishes its own schema id
`HFOC-PRODUCTION-SUPPORT-1` (`APPLIANCE_PRODUCTION_SUPPORT_SCHEMA_ID`), while
the sealed binding keeps the algorithm-independent bundle schema
`HFOC-SB-BIND-1`:

- `publish_bundle` exports into a bounded, worst-case-sized buffer
  (`APPLIANCE_PRODUCTION_SUPPORT_BUNDLE_MAX`, derived from the frozen format
  bounds), re-checks redaction, seals, verifies its own seal and publishes
  `READY` only for a sealed bundle; an unsigned bundle is `DEGRADED`, a
  redaction failure or a missing dependency is `UNAVAILABLE`;
- `break_glass_apply` performs the effects: it publishes `LOCAL_ONLY` through
  the bound ingress policy and revokes the live session; an effect that cannot
  be performed degrades the plane and is counted (`break_glass_partial_count`)
  instead of being reported as success;
- `render` publishes one canonical line
  (`support_plane v=1 state=... bundle=... sections=... withheld=... bytes=...
  session=... session_active=... break_glass=... break_glass_armed=...
  effects=... shell=FORBIDDEN`), and the ITEM-366 management snapshot carries
  the same view as `support=SEALED|UNSIGNED|REDACTION_FAILED|UNAVAILABLE
  support_withheld=N session=ACTIVE|NONE break_glass=ARMED|STANDBY`.

## Proofs

| Proof | Target | What it proves |
| --- | --- | --- |
| Bundle | `make test-appliance-support-bundle-host` | redaction counting, canonical binding, deterministic encoding, real HMAC seal, self-verification, and refusal of every tamper, wrong-key, unsigned, leaked and short-buffer path |
| Session | `make test-appliance-support-session-host` | inert requests, network consent refusal, closed scope vocabulary, local consent, bounded expiry, revocation, fail-closed unavailability, bounded audit |
| Break-glass | `make test-appliance-support-break-glass-host` | declared action set, local-presence-only arming, two-phase single-use token, window expiry, cancel, remote refusal, bounded audit |
| Plane | `make test-appliance-production-support-host` | byte-identical composed bundle, sealed/unsigned states, consent flow, break-glass effects on the real ingress policy and session, partial-effect degradation |
| Management view | `make test-appliance-production-management-host` | the management snapshot and canonical render carry the support view |

## Honest limits

- **Asymmetric signing is not available on device.** The seal is a keyed
  HMAC-SHA-256 tag; an ECDSA signature over the binding requires a private key
  the appliance does not hold. The release signing ceremony (ITEM-380) owns
  that identity, and the binding schema (`HFOC-SB-BIND-1`) is already
  algorithm-independent so a future signature can cover the same bytes.
- **No seal key is bound on ordinary boot.** The production composition
  reports `UNSIGNED reason=NO_SEAL_KEY` until the composition that owns the
  state key material binds one; the state-encryption custody path (ITEM-373)
  currently keeps the DEK inside the loader, and binding it is a separate
  change.
- **Ordinary boot does not compose the plane yet.** The frozen baseline has no
  ordinary-boot metrics/health/status store, so the bundle sources do not
  exist there; the plane is proven at the composition level, and wiring it
  into the end-to-end production path is ITEM-379's scripted run.
- **The support session audit window is volatile.** Entries live in the
  bounded module state and the ITEM-327 ring; there is no persistent audit
  store in this wave (ITEM-377 publishes the same limit).
- **Physical presence proof is the console, not a hardware token.** Local
  presence means the local console principal; a cryptographic presence proof
  (TPM, smart card) is not part of the frozen baseline.
- `VERSION` is not changed by this item.
