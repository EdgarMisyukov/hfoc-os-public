# HFOC Storage Encryption and Key Custody

**Date:** 2026-09-11
**Target:** ITEM-373 of the HFOC Appliance Platform productization wave
**Release contract:** HFOC-APPLIANCE-V1
**Status:** Implementation and focused proof complete on the feature branch;
branch CI, Ready, merge, and post-merge gates are pending.

## Purpose

ITEM-360 gave ordinary boot a state partition with seven fixed stores; ITEM-371
and ITEM-372 made media and the boot chain verifiable. The state volume itself
was still readable and writable in the clear: hostnames, network configuration,
service enablement and, most importantly, the secret store with the
administrator credential, the bearer token and the private key material.

ITEM-373 seals that volume. The installer can bind it to an operator passphrase,
escrow a recovery key, and every store then lives on disk as authenticated
ciphertext. At boot the loader refuses to open the volume without a key, refuses
a wrong key without producing a single plaintext byte, and the production boot
coordinator publishes an honest `LOCKED`/`KEY_REJECTED` readiness instead of a
fabricated ready. TPM sealing is declared, has no backend in the frozen
baseline, and reports `UNAVAILABLE` rather than falling back to a weaker
binding.

## Threat Model

In scope:

- offline disclosure of a powered-off disk (the volume is ciphertext);
- offline modification of a store, a block, a tag entry or the custody record
  (each fails authentication and the read fails closed);
- a wrong passphrase, a wrong recovery key or a foreign volume (no plaintext,
  no writes, honest degraded status);
- replay of an older ciphertext block (each write advances a per-block counter
  that is part of the nonce and of the tag).

Out of scope and honestly `NOT_TESTED`:

- a running-system attacker with the unlocked key in memory;
- firmware, TPM and cold-boot attacks (no TPM backend is bound);
- an attacker who can observe every write to the disk over time;
- denial of service against the disk itself (a destroyed custody block reports
  `CORRUPT`; it is not recovered here).

## Volume Layout

The state partition keeps the ITEM-360 layout unchanged. `layout.block_capacity`
(512 blocks) carries the eight stores; the **last partition block** holds the
512-byte custody record. The installer therefore requires
`partition.block_count > layout.block_capacity`, and a partition without a
custody block is treated as a legacy plaintext volume so existing installs keep
booting.

Each sealed store with `N` physical blocks exposes `D = N - ceil(N / 33)`
authenticated data blocks; the remaining `ceil(N / 33)` blocks at the end of
the store hold 16-byte tag entries (a 32-bit write counter and a 96-bit
HMAC-SHA-256 tag). A 64-block store therefore exposes 62 data blocks, and the
data-region header written at install records `D - 1` exactly like the
plaintext path. A store with more than 256 data blocks is refused as
`UNSUPPORTED` rather than truncated.

```
state partition
+----------------------------------------------------------+------------+
| system hostname | system network | service | certificate  |  custody   |
| secret | diagnostic | recovery metadata                 |  record    |
| 7 x 64 blocks, each = 62 data blocks + 2 tag blocks      |  (512 B)   |
+----------------------------------------------------------+------------+
```

## Cryptographic Composition

- **Data confidentiality:** AES-256-CTR over each 512-byte block, with a
  128-bit nonce counter of `volume_id(8) || block_index_le32 || counter_le32`.
  The write counter is per block and advances on every write, so the same
  plaintext never reuses a keystream.
- **Integrity:** encrypt-then-MAC. A full HMAC-SHA-256 over
  `"HFOC-STATE-BLOCK" || namespace || store_id || block_index || counter ||
  ciphertext` is truncated to 96 bits and stored in the tag entry. The
  ciphertext is authenticated *before* it is decrypted, so a tag failure
  returns an I/O error and leaves the caller's output buffer untouched.
- **Key separation:** the 256-bit data-encryption key (DEK) is per volume. Each
  store derives `enc_key = HMAC(DEK, "HFOC-STATE-ENC" || namespace || store_id)`
  and `mac_key = HMAC(DEK, "HFOC-STATE-MAC" || namespace || store_id)`, so a
  store cannot decrypt or forge another store.
- **Passphrase stretching:** PBKDF2-HMAC-SHA-256 with a 16-byte salt and a
  bounded iteration count (`1024 .. 1000000`, default `20000`). The primitive
  accepts any non-zero count so the RFC vectors stay testable; the custody
  record validates the production bound.  ITEM-611 prepares the HMAC key and
  its two pad states once per derivation; the round copies those states, and
  the prepared material is zeroed on every exit path.
- **Wrapping:** the DEK is wrapped with AES-256-CTR under the derived
  key-encryption key and authenticated with an HMAC-SHA-256 tag; the record MAC
  under the DEK binds every plaintext field.

### Custody record (512 bytes, little-endian)

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 8 | magic `HFOCKEY1` |
| 8 | 4 | version (1) |
| 12 | 4 | binding (`PASSPHRASE`, `RECOVERY_KEY`, `TPM`) |
| 16 | 4 | KDF id (`PBKDF2-HMAC-SHA-256`) |
| 20 | 4 | KDF iterations |
| 24 | 4 | recovery KDF iterations |
| 28 | 4 | reserved (zero) |
| 32 | 16 | passphrase salt (also the wrap nonce) |
| 48 | 8 | volume id |
| 56 | 8 | reserved (zero) |
| 64 | 32 | wrapped DEK (passphrase) |
| 96 | 16 | wrapped DEK tag |
| 112 | 32 | wrapped DEK (recovery key) |
| 144 | 16 | recovery wrap tag |
| 160 | 16 | recovery salt |
| 176 | 32 | DEK fingerprint |
| 208 | 32 | recovery-key fingerprint |
| 240 | 32 | record MAC under the DEK |
| 272 | 240 | reserved (zero) |

The record contains only wrapped key material, salts, fingerprints and a MAC,
so it is safe to keep unencrypted; every reserved byte must be zero and a
non-zero reserved byte is refused as `RESERVED_NONZERO`.

## Install Path

`appliance_state_encryption_install(partition, layout, passphrase, random, ctx,
result)` is the install-time gate:

1. validate the partition (512-byte blocks, one spare block beyond the layout),
   the layout and the passphrase material;
2. generate the DEK, the recovery key, both salts and the volume id from the
   caller-bound randomness;
3. derive both key-encryption keys, wrap the DEK twice, compute the
   fingerprints and the record MAC, and encode the custody record;
4. write the custody block to the last partition block;
5. for every layout entry: bind the range, derive the per-store keys,
   zero-fill every data block through the sealed view (so a fresh block always
   has a valid tag), write the encrypted data-region header and flush;
6. publish the sealed-store count, the DEK and recovery fingerprints, and the
   recovery key as fixed-length uppercase hex in the result.

Randomness failure aborts the install before the custody block is written. The
passphrase is never stored: it is stretched and zeroized in the same call.

## Boot Path

`appliance_state_loader_load_with_key(loader, disk, layout, material)` keeps the
plaintext path unchanged and adds the sealed path:

- `appliance_state_encryption_volume_probe` reads the last partition block.
  Without the magic the volume is legacy plaintext and no key is required. With
  a malformed record the loader reports `ENCRYPTION_CORRUPT` before reading any
  store.
- With a valid record and no material the loader reports `KEY_REQUIRED`; with a
  wrong or tampered key `KEY_REJECTED`; with the TPM binding
  `KEY_UNAVAILABLE`. In all three cases no store is decrypted and the loader is
  not ready.
- With the correct passphrase or recovery key the stores are bound through
  sealed views and the four current stores load normally.

The production boot coordinator maps those statuses:

| Loader status | Boot status | Readiness | Mode |
| --- | --- | --- | --- |
| `OK` | `APPLIANCE_PRODUCTION_BOOT_OK` | `READY` | `NORMAL` |
| `KEY_REQUIRED` | `..._STATE_LOCKED` | `DEGRADED` | `DEGRADED` |
| `KEY_REJECTED` / `KEY_UNAVAILABLE` / `ENCRYPTION_CORRUPT` | `..._STATE_KEY_REJECTED` | `DEGRADED` | `FAIL_CLOSED` |
| anything else | `..._STATE_LOAD_FAILED` | `UNAVAILABLE` | `FAIL_CLOSED` |

The kernel prints the loader status on the existing `[PRODUCTION-BOOT] state=`
line, so an unattended boot of a sealed volume is visibly `KEY_REQUIRED`, not
quietly ready. The caller-bound key provider is
`appliance_production_boot_set_state_key`; ordinary boot leaves it unset until a
TPM or console provider is bound.

## Recovery Key Escrow

The recovery key is 32 random bytes rendered as 64 uppercase hex characters.
`appliance_key_custody_escrow_line` produces the canonical record

```
HFOC-RECOVERY-KEY v1 key=<64 hex> fp=<64 hex>
```

where the fingerprint is `HMAC(recovery_key, "HFOC-CUSTODY-RECOVERY-FINGERPRINT")`.
The operator records that line offline before the appliance leaves the
installer; the recovery key opens the same volume through the
`RECOVERY_KEY` binding, and `appliance_key_custody_recovery_decode` refuses a
malformed or truncated key. The recovery key is never derived from the
passphrase, so losing the passphrase does not lose the volume, and losing both
is an unrecoverable fail-closed state rather than a silent reset.

## Failure Semantics

- **Wrong key:** the wrap tag fails before the DEK exists; the custody state
  stays empty, the DEK buffer is zeroized, no store is bound.
- **Tampered ciphertext or tag:** the recomputed tag does not match; the read
  returns `BLOCK_DEVICE_IO_ERROR` and the output buffer is untouched.
- **Tampered tag entry on write:** the write path authenticates the current
  block against the stored tag before it advances the counter, so a write to a
  store whose tag entry was modified or erased fails closed, leaves the block
  unchanged and cannot force a nonce to repeat.
- **Tampered record MAC:** the DEK unwraps, the fingerprint matches, and the
  record MAC fails as `AUTH_FAILED`.
- **Foreign volume:** the same passphrase on another volume derives a different
  DEK and a different volume id, so the tags do not verify.
- **Unsupported geometry:** a store above 256 data blocks is refused; the
  loader never silently truncates a store.
- **Counter exhaustion:** a block that reaches 2^32 writes fails closed rather
  than reusing a nonce.

## Evidence

| Gate | Target | Marker |
| --- | --- | --- |
| Focused host proof | `make test-state-encryption-host` | `Appliance state encryption host tests: ALL OK` |

The focused proof runs 115 checks and covers:

- RFC 4231 HMAC-SHA-256 cases (including a key larger than the block size);
- PBKDF2-HMAC-SHA-256 known answers (single- and multi-block output) and
  the iteration/output bounds;
- NIST SP 800-38A F.5.5 AES-256-CTR known answers and a non-block-multiple
  round trip;
- custody create/encode/decode/open under the passphrase and the recovery key,
  the escrow line, TPM `UNAVAILABLE`, wrong key, tampered wrap tag, tampered
  record MAC, corrupted magic, non-zero reserved bytes, bad version, bad
  binding and unbounded iteration counts;
- the sealed store adapter: plaintext/ciphertext difference, header decryption,
  reopen after a fresh volume handle, counter advance on rewrite, tampered
  ciphertext and tag fail-closed with an untouched output buffer, a tampered or
  erased tag entry that refuses the next write and leaves the block, and a
  foreign volume that cannot read the ciphertext;
- the encrypted install on a GPT fixture: custody block placement, no
  plaintext header or passphrase on the raw volume, and the loader reporting
  `KEY_REQUIRED`, `KEY_REJECTED`, `KEY_UNAVAILABLE`, passphrase-ready and
  recovery-key-ready, while a legacy plaintext volume keeps the old ready path.

The production end-to-end acceptance stage `STORAGE_ENCRYPTION` names this
target and marker and is `DEGRADED`, because unattended TPM unlocking is
unavailable in the frozen baseline.

## Kernel Integration

- `build/obj/appliance_state_crypto.o` is the one production-kernel object
  compiled with the state crypto profile
  (`kernel/include/hfoc_state_crypto_config.h`: AES-CTR only, no TLS, X.509,
  PK, PSA, MD, HKDF or DRBG family), and the kernel links exactly one Mbed TLS
  source for it, `third_party/mbedtls/library/aes.c`. HMAC and PBKDF2 are built
  on the HFOC SHA-256 implementation.
- `MBEDTLS_PLATFORM_ZEROIZE_ALT` is declared by the profile and defined once
  per image: the production kernel takes the ITEM-372 definition, and the host
  proof defines its own.
- Every other build (`fallback`, the macro-gated proof kernels) compiles the
  bounded `UNAVAILABLE` stubs and links no cipher, so a proof kernel cannot
  silently grow a crypto dependency.
- `PRODUCTION_CFLAGS` gains `-DHFOC_PRODUCTION_STATE_ENCRYPTION`; the loader,
  the boot coordinator and their host proofs use `HFOC_STATE_ENCRYPTION_HOST_TEST`.

## Limitations

- **TPM sealing is not implemented.** The binding is declared, the record can
  carry it, and every TPM path reports `UNAVAILABLE`/`KEY_UNAVAILABLE`; a
  physical TPM proof is `NOT_TESTED`.
- **No unattended key provider on ordinary boot.** A sealed volume boots
  `STATE_LOCKED` with `DEGRADED` readiness until a TPM or console provider is
  bound; the QEMU proofs that expect a ready plaintext state are unaffected.
- **No rekey or rotation yet.** Changing the passphrase means re-wrapping the
  DEK; there is no in-place rekey operation in this item.
- **No key escrow server.** Escrow is the operator-recorded canonical line.
- **The tag table is not a Merkle tree.** Per-block authentication detects
  modification, not truncation of a whole store; store-level capacity checks
  bound the damage.
- **`VERSION` is not changed** and no v1.0.0 is created.

## Non-Goals

- DHCP/DNS, services/HTTPS, the panel and update operations (later items).
- A TPM 2.0 command stack, measured-boot sealing or remote attestation.
- Encryption of the ESP, the recovery partition or the ISO itself.
- Any change to APIC/IOAPIC/SMP and any custom UEFI loader.
