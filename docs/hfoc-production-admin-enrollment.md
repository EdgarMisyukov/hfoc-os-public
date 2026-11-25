# HFOC Production Administrator Credential Enrollment

**Date:** 2026-09-13
**Target:** ITEM-401 of the HFOC Appliance Platform productization wave
**Release contract:** HFOC-APPLIANCE-V1
**Status:** Implementation, host proof and authenticated QEMU proof on the
feature branch; branch CI, Ready, merge and post-merge gates are pending.

## Purpose

ITEM-400 made the read-only management targets real on an ordinary production
boot, but the ITEM-365 request gate still starts every boot without a verifier:
the ITEM-363 first-boot flow commits only the SHA-256 *fingerprint* of an
administrator credential to the `secret.admin.credential` record, so every
protected target answers `503` and the authenticated bodies are unreachable on
a real appliance.

ITEM-401 is the missing owner between the persisted fingerprint and the live
gate:

- `kernel/src/appliance_production_admin_enrollment.c` accepts the account
  (login and password) from the local console and the remote panel, derives the
  32-byte PBKDF2 verifier in memory, publishes it into the ITEM-365 gate and
  issues one bounded bearer session.  ITEM-506 increment 1 replaced the
  ITEM-401 single-SHA-256 fingerprint with a salted PBKDF2-HMAC-SHA256 record
  (salt and iteration count stored beside the verifier), and increment 2 added
  the one setter, the one verifier entry and the pure conversation;
- `kernel/src/console.c` gains the one account command family
  (`kernel/include/console_credentials_command.h`): `CREDENTIALS` asks the
  current password, the new login and the new password twice, `LOGIN` opens the
  local session, and `CREDENTIALS RESET` / `CREDENTIALS RESET CONFIRM` keep the
  ITEM-465 break-glass gesture.  Neither command takes an argument, the typed
  password is never echoed, never written to the console history and zeroed
  after every step, and the resulting bearer token is printed once as an
  `Authorization: Bearer` header value.  ITEM-454's case rule still holds: the
  names are matched without regard to ASCII case;
- ordinary boot binds the owner and its salt source to the live gate and the
  console and publishes the `[ADMIN-ENROLLMENT]` record from the start, so the
  appliance states whether it is `UNPROVISIONED`, `AWAITING_SECRET` or `ACTIVE`
  instead of claiming an authenticated plane it does not have;
- the ITEM-321 authorization route table gains the `/api/v1/health` target the
  ITEM-400 owner serves and resolves it through the existing status-read
  system-status entry. Without that mapping the owner resolved the body but the
  gate default-denied a valid administrator session with `403`; the mapping is
  a read-only alias of an existing capability, adds no capability and never
  makes the target anonymous.

## Derivation contract

ITEM-401 originally derived a single unsalted SHA-256 verifier:

```
verifier = SHA-256("HFOC-ADMIN-CREDENTIAL-V1:" || secret)
record   = appliance_secret_protection_fingerprint(
               APPLIANCE_SECRET_PROTECTION_KIND_ADMIN_CREDENTIAL,
               generation 1, verifier, 32)
token    = 32 characters of [A-Za-z0-9_-] derived from
           SHA-256("HFOC-ADMIN-TOKEN-V1:" || verifier || now_tick || counter)
```

ITEM-506 increments 1 and 2 superseded the verifier half of that contract for
the account while keeping the gate and the token builder:

```
login    = 1..32 characters of A-Za-z0-9._-, case-significant
password = 12..128 printable ASCII, no trimming, bounded weak list refused
salt     = 16 bytes from the bound hardware entropy owner (NO_RANDOM without it)
record   = pbkdf2-sha256$<iterations>$<salt-hex>$<verifier-hex>$<login>
           written under the same `secret.admin.credential` key
token    = 32 characters of [A-Za-z0-9_-] derived from
           SHA-256("HFOC-ADMIN-TOKEN-V1:" || verifier || now_tick || counter)
```

The ITEM-401 derivation remains readable as a legacy record: it is refused as
an account (`LEGACY_RECORD`) and is never reinterpreted.  The `CREDENTIALS`
change does not migrate it either - the operator did set that secret once and it
must not be overwritten without proof - so the change is refused and names the
explicit `CREDENTIALS RESET` / `CREDENTIALS RESET CONFIRM` gesture, after which
the account is set again.
The token builder is unchanged, so an existing gate path keeps its shape; only
the verifier it is given changes.

## The credential record and the deferred first boot

Before ITEM-403 the ordinary first-boot flow committed the fingerprint of a
fixed factory credential (`HFOC-APPLIANCE-V1-PROVISIONING-1`) because no
operator secret existed at that point. ITEM-403 removes that commit: ordinary
first boot now defers the administrator stage
(`appliance_production_first_boot_run_deferred`) and publishes
`[PRODUCTION-FIRST-BOOT] … admin=deferred`, so a fresh appliance has **no**
credential record at all.

ITEM-506 increment 4 gives a fresh appliance a second way to get the account:
the operator install screen, after the accepted disk warning, asks for the
login and the password twice and the install writes the same PBKDF2 record
before it reboots, publishing `[PRODUCTION-INSTALL] admin=SET login=<login>
persisted=VERIFIED`.  The record is written into the installed system's state
and read back through the installed system's own reader, so the run cannot
promise `admin=SET` over a store that does not hold it; an unattended install
(or a cancelled screen) writes no administrator record and says `admin=UNSET
reason=UNATTENDED` / `reason=OPERATOR_DECLINED`.  The installer kernel has no
hardware entropy owner, so its salt is a fresh derivation over a domain, the
chosen target disk's identity, the boot-relative tick count and a per-run
counter; the record format and the policy are exactly the console's.

The enrollment owner treats the record states honestly:

- an initialized store with no record is `UNPROVISIONED` and `CREDENTIALS`
  commits the operator's login and PBKDF2 verifier;
- a legacy factory fingerprint (an appliance provisioned by an older build) is
  also `UNPROVISIONED`: `LOGIN` with the factory secret is refused and the gate
  is never activated with it, and `CREDENTIALS` replaces the record;
- once a real structured record exists, the old secret-only entry points refuse
  with `CREDENTIAL_REQUIRED` and only `LOGIN` with the right login and password
  publishes the verifier; a legacy fingerprint record is refused by the
  credential path as `LEGACY_RECORD`, and because it is a real secret the
  appliance still serves, `CREDENTIALS` refuses it too and names the explicit
  reset instead of overwriting it unproven;
- until an operator sets the account, the gate keeps refusing every protected
  target and the appliance publishes that honestly.

## What to do if the password is forgotten (ITEM-465, ITEM-506)

Until ITEM-465 the third bullet above was a dead end: an operator who forgot
the password could not change it and had no reset, so only reinstalling the
appliance brought the panel back. The local console owns the recovery, as the
two-line gesture under the one command family

```
CREDENTIALS RESET
CREDENTIALS RESET CONFIRM
```

The first line only arms the reset and answers
`[ADMIN-ENROLLMENT] reset requires confirmation; type \`CREDENTIALS RESET
CONFIRM\` on the next line`. The second line performs it; it must be exactly
`CREDENTIALS RESET CONFIRM` and must immediately follow the request - any other
submitted command disarms it, and a confirmation with nothing armed is refused,
not honoured. The console publishes
`[ADMIN-ENROLLMENT] reset; state=UNPROVISIONED` and then the full
`[ADMIN-ENROLLMENT] v=1 state=UNPROVISIONED gate=CLOSED provisioned=0 …`
record, both on the serial stream and in the durable journal.

The reset rewrites the persisted `secret.admin.credential` record to the
factory placeholder, so the appliance is back in the deferred first-boot state
and `CREDENTIALS` works again, and it resets the production request gate, which
closes **every** session issued before the reset - a bearer token copied out
earlier stops authenticating. The gesture is local-only: no management-plane
route or body names it, the gate stays closed until a new account is set, and a
network client cannot reach this path.

If the console cannot rewrite the record or cannot reset the gate, the reset is
refused with the named status instead of reporting a success that did not
happen.

## Honest evidence

| Condition | Enrollment state | Gate |
| --- | --- | --- |
| No secret store | `UNAVAILABLE` | closed |
| No credential record (fresh ordinary boot) | `UNPROVISIONED` | closed, settable |
| Legacy fingerprint record | credential path `LEGACY_RECORD` | closed, old secret refused |
| Structured record, no password presented this boot | `AWAITING_SECRET` | closed |
| Right login and password, verifier published | `ACTIVE` | published, bounded session issued |
| Wrong login or password / mismatch | state unchanged | closed-session refusal, failure counted |
| Failure limit reached | unchanged | `THROTTLED` until the cooldown elapses |

ITEM-506 increment 5 makes the same state the gate of the management plane: on
a boot whose state volume is ready, an appliance without a structured account
does not raise its listener at all.  It publishes
`[PRODUCTION-SERVING] state=AWAITING_CREDENTIAL reason=admin_credential_unset
listener=down`, and the console conversation that sets the account lets the
next poll raise it.  An empty store, the factory placeholder and a legacy
fingerprint all withhold the listener; a boot with no ready state volume keeps
serving with a per-boot certificate.

ITEM-506 increment 6 makes the account changeable from the management panel.
The Account page (`/panel/account`, and the scriptable twin
`/api/v1/system/account`) stages the current password, the new login and the new
password twice through the same two-phase mutation plane the network, routing
and hostname changes use: the stage proves the current password with the
owner's verify-only entry, validates the new pair with the credential model's
policy and holds the candidate; the confirmation token is what writes.  The
commit calls `provision_credential()`, the single setter, which draws a fresh
salt, derives the new PBKDF2 verifier, writes the durable record, republishes
the verifier through the request gate and **closes every issued session** - the
cookie and the bearer that authorized the change included.  The appliance
publishes `[PRODUCTION-CREDENTIAL-SET] source=PANEL login=<login>
changed=LOGIN,PASSWORD sessions=REVOKED persisted=OK` (the changed list is
`PASSWORD` alone when the login did not move); the record never carries the
password or a hash of it.  The change signs the operator out of the panel, so
the 303 that answers the confirmation is the last answer that session receives.
A legacy fingerprint record is refused by name (the console's `credentials
reset` is still the migration gesture), an unbound owner or a failed write
answers `APPLIED`-less refusal and leaves the account unchanged, and the panel
never exposes the break-glass reset.

The module never stores, renders or logs the presented password; the console
bridge zeroes the account line after every step, skips the history and the
saved draft, and paints the login plus one star per accepted password character
on the framebuffer only.  The serial console still never shows a credential
answer: ITEM-610 replaced the old no-feedback rule with a visible mask while
keeping the ITEM-506 rule that a password byte reaches no transcript, and the
narrow safety net that strips the argument after the two removed
`admin-provision` / `admin-login` spellings that older printed manuals still
teach stays.  The panel change holds the new password in
a bounded candidate only for the confirmation window (300 s) and zeroes it on
the commit, on a cancel, on the next plane request that observes the expiry, on
a new stage (which discards the candidate it replaces) and on stop/fail-closed;
it is never rendered, never logged and never copied into the audit ring.

## What is proven where

| Claim | Proof |
| --- | --- |
| Derivation, fingerprint checking, factory refusal, provisioning, throttling, session issuance, request authorization and zero heap allocation | `make test-appliance-production-admin-enrollment-host` (234 assertions, `Appliance production admin enrollment host tests: ALL OK`) |
| The local console provisions the record on the ordinary production image | `make test-production-service-https-qemu` (`provision` phase over the QEMU monitor) |
| The provisioned fingerprint survives a reboot, the gate starts closed again, and the local console enrolls the persisted record | `make test-production-service-https-qemu` (`TLS-PROOF rebooted gate: protected target refused before enrollment`) |
| The five authenticated read-only bodies are served with live evidence and the anonymous/unknown-bearer requests stay refused | `make test-production-service-https-qemu` (`TLS-PROOF authenticated read-only management bodies: 200 live evidence`, `TLS-PROOF enrollment gate: anonymous and unknown bearer refused`) |
| The anonymous gate refusal is unchanged before enrollment | the existing `TLS-PROOF rebooted gate: protected target refused before enrollment` (503) phase of the same proof, and the five-target `network-apply boot` phase |
| The serving listener is withheld until the account exists, and rises after the console sets it | `make test-production-service-https-qemu` (`TLS-PROOF serving gate: listener withheld until the account is set`, then `listener live after the account was set`) |
| The managed targets answer 401 to an anonymous request once the verifier is published, and keep the gate-unavailable 503 while a persisted account is not published on the boot | `make test-production-service-https-qemu` (`read-only management targets: known routes, anonymous denial 401`, `network-apply boot: 5 managed target(s) refused with the gate-unavailable 503`) |
| The predicate itself: only a structured account allows serving | `make test-appliance-production-admin-enrollment-host` (`test_first_run_serving_gate`, all four store heads) |
| An installed unattended state carries no account, and the commit is what the gate then reads | `make test-production-installer-orchestration-host` (`test_installed_state_first_run_gate`) |
| An unattended install is provisioned on the first run of the installed disk | `make test-production-forward-qemu` (first-run console set before the relay wait) |
| The panel changes the account and the previous cookie and bearer stop authenticating | `make test-production-service-https-qemu` (`TLS-PROOF account change: the previous cookie is revoked`, `TLS-PROOF account change: the previous bearer is revoked`) |
| The change survives a reboot: the old pair is refused, the new pair signs in and the Account page names the new login | `make test-production-service-https-qemu` (`TLS-PROOF account durability: the old credential is refused after the reboot`, `TLS-PROOF account durability: the Account page names the new login`) |
| The single setter is the invalidation: an old session stops authorizing, the old password stops proving and a fresh salt is drawn | `make test-appliance-production-admin-enrollment-host` (`test_change_revokes_sessions`) |
| The account mutation executes only through a bound executor and refuses a failed write | `make test-appliance-production-mutation-plane-host` (`test_account_executor`) |
| The Account page renders the login and both forms, and the form parser refuses a stray field, a malformed escape and an over-long value | `make test-appliance-management-web-ui-host`, `make test-panel-login-host`, `make test-appliance-management-panel-host` |
| The production kernel still builds and boots | `make check`, `make build/hfoc-kernel.elf`, `make iso` |

## ITEM-516: the durable after-boot policy (variant "b")

ITEM-401 withholds the management listener until a local `LOGIN` publishes the
verifier in memory.  The owner's decision of 2026-10-10 (variant "b") adds one
durable setting that the operator can save once and that every later boot reads:
the third optional key of the system record, `v1:security.panel.after-boot=open`.

**The record.**  `closed` is the default and is never written, so a machine that
never enabled the policy writes exactly the bytes the previous format wrote and
an older build still reads its hostname.  The value grammar is `open|closed`;
anything else is refused by name
(`APPLIANCE_CONFIG_VALIDATION_BAD_PANEL_AFTER_BOOT`) and never treated as the
default.  `appliance_persistent_config_set_hostname()` and `set_language()`
carry the key forward, and the integrity catalogue knows it.

**What the boot says.**  With the key `open` the boot publishes the verifier the
stored structured credential already holds - the same value a successful local
login publishes, so the password, the failure count and the lockout are not
bypassed - and names the decision:

    [PRODUCTION-SECURITY] gate=PUBLISHED source=POLICY

With the default record the listener is withheld exactly as before and the
record says why:

    [PRODUCTION-SECURITY] gate=CLOSED reason=AWAITING_LOCAL_SECRET

An absent store, the factory placeholder and a pre-ITEM-506 legacy fingerprint
have no verifier to publish and refuse by name, so such a boot reports the
closed gate instead of pretending the policy ran.

**Enabling is local-only and proof-bound.**  The console command `gate` shows
the setting and what it means; `gate open` prints the full warning and arms a
confirmation; `gate open confirm` on the very next line writes `open`; any
other submitted line disarms it; `gate close` writes the safe default at any
time.  The confirmation additionally requires a local secret proven in this
console session - a successful `login`, or the current-password step of
`credentials`.  A remote panel sign-in publishes the verifier but never sets
that flag, so there is no anonymous or network path to the dangerous direction.

The warning is contract text and names, in the operator's language: that the
machine no longer requires physical presence; that the panel answers over the
network right after boot; that the password and the five-failure lockout are
the only protections left; and - audit finding R01 - that the management
service listens on a wildcard address, so on an appliance exposed to the
Internet the enabled setting means the panel is open to the whole network.

**Disabling is the panel's.**  `/panel/gate` (and the scriptable
`/api/v1/system/gate`) is an ordinary two-phase mutation: one `PANEL_GATE`
command id (18) in the authorization matrix, whose capacity moves from 16 to
17, and one bound executor on the mutation plane.  The page renders the setting,
the warning and a single `action=close` form; an attempt to open is refused by
name (`GATE_LOCAL_SECRET_REQUIRED`, or `GATE_ACTION_UNKNOWN` for any other
word) before a token is issued, and nothing durable is written.

**Evidence.**  `make test-appliance-panel-gate-host` (both boot records byte for
byte), `make test-appliance-production-admin-enrollment-host` (the policy
publication opens the serving gate with no authentication; a missing store, an
empty store and a legacy fingerprint refuse by name), `make test-console-host`
(the grammar, the refusal without a proof, the warning sentences, the write on
the confirmation, the safe direction without any proof), the three config
proofs (the record, the default bytes, the recovery of an unknown value),
`make test-appliance-production-mutation-plane-host` (the two-phase machine and
the refused open), `make test-appliance-panel-forms-host` (the drawn page has
one close-only action form and no control that opens), the authorization, panel
and web-UI proofs, and `make test-production-service-https-qemu`: the default
boot publishes the closed record and keeps answering `AUTH_UNAVAILABLE` before a
local login, the local console enables the policy after a real `login`, and the
next boot publishes `gate=PUBLISHED source=POLICY` and answers over TLS with no
local login at all.

**Residual risk (stated, not hidden).**  An enabled policy removes the
physical-presence requirement: until the next `gate close` the panel answers
over the network right after boot, and the listener is declared on a wildcard
address (R01), so an appliance exposed to the Internet exposes the panel to the
whole network.  The password and the failure lockout are the only protections,
the operator must read that before confirming, and neither the panel nor a
script can enable the setting.

## Limitations

- **The session is bounded per boot.** The token lives only in the ITEM-320
  session table for the configured lifetime; a reboot returns the appliance to
  `AWAITING_SECRET` and requires the account again - unless the ITEM-516
  after-boot policy is enabled, in which case the boot publishes the stored
  verifier itself and the panel answers without any local login. The panel's
  cookie session is the ITEM-445 browser session and expires with it.
- **The console hides the typed password now.** ITEM-506 removed the echo of
  the ITEM-401 era; the trade is that the local break-glass channel no longer
  shows what was typed, which is exactly what the account policy requires.
- **Legacy fingerprint records are refused, not rewritten.** An appliance that
  booted an older build still carries its old record; `CREDENTIALS` refuses it
  with `LEGACY_RECORD` because the operator did set that secret once and it
  must not be overwritten without proof, so the operator takes the explicit
  `CREDENTIALS RESET` / `CREDENTIALS RESET CONFIRM` gesture before setting the
  account again.  Since ITEM-506 increment 5 such an appliance also withholds
  its management listener until that gesture and the new account: a legacy
  fingerprint is not an account the panel can sign in with, so serving it would
  advertise a plane nobody can enter.
- The token derivation uses `hfoc_lwip_time_now()` as the per-boot salt, so the
  token never repeats across boots while the credential is unchanged.

## Contracts

No test, verifier or `check.sh` contract was weakened. The new host proof joins
the canonical host suite, the request gate keeps its admission order and its
unavailable-by-default behaviour, and the QEMU proof adds an authenticated
phase without removing the anonymous refusal phase.
