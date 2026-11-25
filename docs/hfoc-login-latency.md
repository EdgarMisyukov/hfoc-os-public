# Local login latency — measurement and the proved extra work

ITEM-611 continues ITEM-610 (`361f8fb6`). The owner reported that a local
password check takes very long. This page records what the production path
actually does, which extra work was proved removable, and what remains open.

## The path

Ordinary boot with a provisioned host seed opens the normal credential
conversation. `kernel_production_entropy_unlock()`:

1. verifies the account through `appliance_production_admin_enrollment_verify_credential_pair()`;
2. unlocks the two-slot host seed with `appliance_host_seed_store_unlock()`
   (read both slots, authenticate, write the next epoch, flush, read back and
   authenticate the readback);
3. publishes the already-proved account without re-running the account KDF;
4. derives the DRBG material from the new seed.

The account proof uses PBKDF2-HMAC-SHA-256 at the stored iteration count
(default 200,000, verifier 32 bytes). The seed slots store their own iteration
count (20,000 in the installed record) and a 64-byte PBKDF2 output (two
32-byte blocks).

## Trace

The production kernel emits one bounded record from the unlock path:

```
[LOGIN-TRACE] v=1 build=<id> available=1 attempt=<n> ok=0|1 stage=<n>
  account_us=... account_iterations=...
  seed_us=... seed_reads=... seed_writes=... seed_opens=... seed_seals=...
  seed_readbacks=... seed_kdf_calls=... seed_kdf_iterations=...
  seed_read_us=... seed_open_us=... seed_seal_us=... seed_write_us=...
  seed_readback_us=... publish_us=... entropy_us=... total_us=...
```

The durations come from `timer_cycles()` (TSC), not from the PIT tick, so a
synchronous KDF cannot stop its own clock. The record carries no password,
seed, tag, key or hash-derived value. `scripts/test-qemu-host-provisioned-entropy.sh`
parses it on the no-RDRAND/no-RDSEED installed boots and requires the account
iteration count, at least two seed opens, one seal and three seed KDF calls on
a successful login.

ITEM-614 keeps the last **finished** attempt as bounded scalar data, so the
same record is rendered to serial, the boot journal and
`kernel_production_status_report()` -> `network`/Status/`diag save`/SSH
`hfoc diag`. Before any attempt the shared record is
`available=0 state=NOT_ATTEMPTED`; while a new attempt runs it is
`available=0 state=IN_PROGRESS` and never repeats the previous success. A
failed attempt replaces the previous result with its own `ok=0` and failure
stage. Composing a snapshot performs no authentication, KDF or epoch write:
it only reads the finished result.

## Measured before the fix

QEMU `qemu64` TCG, UEFI install, no hardware RNG. These are guest microseconds,
not Toshiba time:

| stage | boot 1 | boot 2 |
|---|---:|---:|
| account verify + KDF | 7,899,729 us | 9,618,176 us |
| seed unlock | 16,111,722 us | 22,230,561 us |
| seed opens / seals | 3 / 1 | 4 / 1 |
| seed KDF calls / iterations | 4 / 80,000 | 5 / 100,000 |
| total unlock | 24,131,617 us | 31,928,258 us |

The seed I/O was already negligible (`seed_read_us=511`, `seed_write_us=188`,
`seed_readback_us=58` in boot 1). The cost was the KDF.

## Root cause

`appliance_state_crypto.c::state_crypto_hmac()` rebuilt the HMAC key block and
both inner/outer pad blocks and hashed both pads **on every call**. PBKDF2 calls
it once for the first block and once for every subsequent round, for each output
block, so the seed path paid two extra SHA-256 compressions per round. The
account KDF had already been changed to prepare the key once
(CI-SPEEDUP-4), but the state-crypto KDF used by the host seed had not.

A second, smaller redundancy was in `appliance_host_seed_store_unlock()`: the
scan authenticated the selected slot, and the code then opened the same slot a
second time before sealing the next epoch, re-running the password KDF for
bytes it had already verified.

## Fix

- `appliance_state_crypto.c` prepares one `state_crypto_hmac_key` per PBKDF2
  derivation: the key block is normalised once, `ipad`/`opad` are absorbed once
  and a round copies the two SHA-256 states. The prepared state is zeroed on
  every exit path. Vectors, iterations, salt, output length and the derivation
  are unchanged.
- `appliance_host_seed_store_unlock()` reuses the already-authenticated
  `candidates[selected]` record and its sequence instead of opening the
  selected slot again. The readback of the newly written slot is kept, so a
  torn or substituted advance still fails.

Nothing else moved: the account 200,000-iteration default, the seed-slot
20,000 default, the record format, the account/seed MAC and device binding, the
epoch advance, flush/readback, lockout, constant-time comparisons and the
failure stages are unchanged.

## Measured after the fix

| stage | boot 1 | boot 2 |
|---|---:|---:|
| seed unlock | 4,200,507 us | 12,330,598 us |
| seed opens / seals | 2 / 1 | 3 / 1 |
| seed KDF calls / iterations | 3 / 60,000 | 4 / 80,000 |
| total unlock | 12,182,801 us | 27,712,445 us |

The seed KDF work falls from 4 to 3 derivations (80,000 to 60,000 iterations)
on the first login and from 5 to 4 (100,000 to 80,000) on the next boot; the
per-derivation compression cost is also halved by the prepared key. Boot 2's
`account_us` moved from 9.6 s to 15.3 s between runs on the TCG guest, which is
why the total is noisier than the seed stage. A repeated after-fix run on a
loaded host measured seed 9.8 s / 20.8 s while the KDF and operation counts
stayed exactly 3/60,000 and 4/80,000; only the guest wall time moved. The
account KDF remains the largest single stage and is deliberately **not**
reduced: iterations, salt and verifier length are the security parameter.

## Host proof

`make test-state-encryption-host` builds with the SHA-256 host counter and
requires a 20,000-iteration, 64-byte derivation to stay within the prepared-key
budget (two pad compressions + two per HMAC) instead of the old pad-per-round
count. The RED control against the pre-fix file fails exactly this assertion.
The same host suite keeps all RFC 4231/PBKDF2 vectors, and
`tests/appliance-host-seed-old-slot.h` is a slot sealed by the **old** code that
the new code still opens (epoch 1 -> 2, same seed) without reprovisioning.

## What remains open

- The physical Toshiba duration is still unknown. Host/QEMU time is not
  Toshiba time. After the ITEM-614 medium update the owner performs an ordinary
  login, then `diag save` on the diagnostic flash. The saved session carries the
  build-matched `[LOGIN-TRACE]` (or `available=0` with its named state), so the
  Debian extractor reads it without a photograph or serial adapter.
- The account KDF is intentionally expensive; if the owner still finds the
  physical wait too long after this fix, the next step is a measured hardware
  trace and a decision about the security parameter, not a silent reduction.
- The seed path still performs three derivations on the first login (selected
  slot, readback authentication, next-epoch seal) and four on a later boot
  (two populated slots, readback, seal). Those are the remaining
  state-ordering/anti-rollback checks, not extra work.
