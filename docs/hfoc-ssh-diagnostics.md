# HFOC SSH diagnostics and delivery

This page is the operator's short path for the owner-approved SSH diagnostics
endpoint (ITEM-601..ITEM-604). It does not replace `AGENTS.md`, the hardware
acceptance runbook or the local console's own instructions.

## Endpoint

- Real SSHv2 on the management IPv4/port 22; publickey-only user `hfoc`.
- Exactly four commands: `hfoc status`, `hfoc network`, `hfoc journal`,
  `hfoc diag`. Shell, PTY, SFTP/SCP, forwarding, agent/X11, env, subsystem and
  every other spelling are refused.
- `hfoc diag` streams the same record composition `diag save` verifies, framed
  with the shared BEGIN/END/session/count/CRC and an explicit
  `mode=STREAM saved=no` trailer. It does not write a USB medium.
- The endpoint starts only after the ordinary network, entropy, account and
  host-key gates; it never bypasses the local `HOST_PROVISIONED` unlock.

## Enrollment from Debian (no long manual input)

The supported first-run path carries the client's **public** key on the
state-preserving update medium; the private key never leaves Debian.

1. On Debian, create the client key if it does not exist:
   `ssh-keygen -t ecdsa -b 256 -f ~/.ssh/hfoc_diag -N ''`.
2. Note the client-key fingerprint the appliance will confirm:
   `ssh-keygen -lf ~/.ssh/hfoc_diag.pub` (for example
   `256 SHA256:... hfoc_diag (ECDSA)`).
3. Build the update medium from the product tree with the public key and that
   fingerprint. Only the public file is read:

   ```
   make production-installer-update-media \
       HFOC_INSTALL_MEDIA_CONFIRM=ITEM-394-UNATTENDED-INSTALL \
       HFOC_INSTALL_MEDIA_TARGET=TESTNVME000001 \
       HFOC_INSTALL_MEDIA_LAYOUT=update \
       HFOC_INSTALL_MEDIA_MANAGEMENT=management-interface \
       HFOC_INSTALL_MEDIA_NETWORK=dhcp \
       HFOC_INSTALL_MEDIA_SSH_KEY=$HOME/.ssh/hfoc_diag.pub \
       HFOC_INSTALL_MEDIA_SSH_KEY_CONFIRM=SHA256:...
   ```

   The medium build refuses a key without the matching confirmed fingerprint,
   a non-ECDSA-P256 key, or a fingerprint without the key. The ISO carries only
   the 88-character public point as a Limine module, and its install token
   names the confirmed fingerprint.
4. Boot the medium. The installer writes the key into the installed state,
   flushes it and reads it back through the installed reader, and publishes
   `[PRODUCTION-INSTALL] ssh-key fingerprint=SHA256:... persisted=VERIFIED`.
   A write or read-back failure refuses the run instead of promising a key the
   next boot does not have.
5. After the reboot, once the SSH endpoint is prepared, `ssh-key show` on the
   appliance must print the same `authorized=SHA256:...` fingerprint. No
   88-character text is typed.

The local console path remains available as a fallback: `ssh-key add` stages
the 88-character base64 point and `ssh-key confirm <fingerprint>` writes and
reads it back. `ssh-key remove` revokes the key; `credentials reset` also
revokes it. A failed durable clear keeps the listener stopped and is retried.
The live RAM authorization is revoked first, so the console reports
`state=RAM_REVOKED durable=FAILED; the key may return after reboot` when the
store cannot be written; a store that can never be written cannot promise a
cross-reboot tombstone, and the console does not claim one.

The private key is never passed in argv, copied to the medium, or printed by
the collector.

## Pin the host key without trusting the first scan

`ssh-keyscan` is only used to fetch a candidate; the console fingerprint is the
trust anchor:

```
ssh-keyscan -t ecdsa 192.0.2.13 > /tmp/hfoc.scan
ssh-keygen -lf /tmp/hfoc.scan          # compare with `ssh-key show`
```

Only when the printed fingerprint equals the console's, install the file as the
pinned known_hosts:

```
install -m 600 /tmp/hfoc.scan ~/.ssh/hfoc_known_hosts
```

Do not use `StrictHostKeyChecking=no` or `accept-new`.

## Run one command

```
ssh -i ~/.ssh/hfoc_diag \
    -o IdentitiesOnly=yes \
    -o UserKnownHostsFile=$HOME/.ssh/hfoc_known_hosts \
    -o StrictHostKeyChecking=yes \
    -o BatchMode=yes \
    hfoc@192.0.2.13 'hfoc status'
```

The collector is the same session with private output, a pinned known_hosts and
diag verification:

```
scripts/hfoc-ssh-collect.sh \
    --host 192.0.2.13 \
    --key ~/.ssh/hfoc_diag \
    --known-hosts ~/.ssh/hfoc_known_hosts \
    --out ~/hfoc-diag \
    --command diag
```

It writes a `0700` directory and `0600` files, prints the report path, and
records `ssh_exit=0 verified=yes build=... session=... records=... crc=...` in
the sidecar `.status` file. A nonzero SSH status, an empty body, truncation, a
missing/duplicated END, a count/CRC mismatch, a missing STREAM/saved=no trailer
or a `SAVED` claim is a failure; partial output is kept under `*.failed`.

## State-preserving update medium

The update medium is the unattended entry with `LAYOUT=update`; it reaches the
preflight that proves the persistence range and the installed reader before it
writes anything. Build it with the owner's declared target:

```
make production-installer-update-media \
    HFOC_INSTALL_MEDIA_CONFIRM=ITEM-394-UNATTENDED-INSTALL \
    HFOC_INSTALL_MEDIA_TARGET=TESTNVME000001 \
    HFOC_INSTALL_MEDIA_LAYOUT=update \
    HFOC_INSTALL_MEDIA_MANAGEMENT=management-interface \
    HFOC_INSTALL_MEDIA_NETWORK=dhcp
```

The build is heavy and goes through `scripts/hfoc-heavy-lock.sh local`. Writing
the ISO to a USB stick or the SSD is a separate owner-authorized operation; the
automation never starts it.

### Verify the update path on the stock QEMU fixture, not on a personalised ISO

The standard regression runs on the media the target builds for the fixture:

```text
scripts/hfoc-heavy-lock.sh local -- make test-production-installer-update-qemu
```

That target builds
`build/production-installer/hfoc-os-1.0.0-production-installer-update.iso` with
the default `HFOC_INSTALL_MEDIA_TARGET=QM00001`, which is the serial the proof
attaches to its own target disk (`-device ide-hd,...,serial=QM00001`). Do not
substitute the owner's `TESTNVME000001` ISO into this command: the production
identity guard correctly refuses it and publishes

```text
[PRODUCTION-INSTALL] refused reason=TARGET_NOT_FOUND target=TESTNVME000001 writes=0
```

A terminal refusal like that is an answer from the production path, not a
missing success marker. The proof's waiter ends the positive phase with the
refusal line at once (ITEM-609); it no longer waits out the `applied` deadline.
A personalised update ISO is verified as a separate artifact - token fields
without printing the confirmation value, staged-vs-ISO identity, installed
configuration, `HFOCSIGN` manifest/chain, both kernel build ids and
`scripts/hfoc-media-image-check.sh` - and is never a QEMU fixture image.

### ITEM-604 build record (2026-11-14, base `e17a9df9`)

- ISO: `build/production-installer/hfoc-os-0.2.6-production-installer-update.iso`
- size: `38344704` bytes
- SHA-256: `519a9525f53ba3ce79279012f7bc00afcce765557999528c8dc02ff800b6c1bb`
- token: present; fields `CONFIRM`, `TARGET=TESTNVME000001`,
  `MANAGEMENT=management-interface`, `LAYOUT=update`, `NETWORK=dhcp`
  verified by exact-line checks without printing the token.
- boot entry: first entry is `/HFOC install (unattended, media-authorised)`
  with `cmdline: hfoc.install=unattended`; three entries total.
- installed configuration: `hfoc.management=management-interface
  hfoc.network=dhcp`.
- sealed media manifest and installed chain are `HFOCSIGN` envelopes.
- both kernels (`hfoc-installer-kernel.elf`, `hfoc-kernel.elf`) carry build id
  `e17a9df9b9fc`; their SHA-256 digests differ, so the installer and the
  installed kernel are distinct.
- the token extracted from the ISO matches the staged token byte for byte.
- `scripts/hfoc-media-image-check.sh` accepted the image digest and build id.

### Final delivery medium (main `42647a98`, 2026-11-14)

- ISO: `build/production-installer/hfoc-os-0.2.6-production-installer-update.iso`
- size: `38410240` bytes
- SHA-256: `c56e4c019d02dbc09c80c0cf2fe8b4235f3b1ceaea2ee34e8c59458189e4d7ed`
- main/build id: `42647a98068f`; both `hfoc-installer-kernel.elf` and
  `hfoc-kernel.elf` carry it, with distinct digests.
- token: present; `CONFIRM`, `TARGET=TESTNVME000001`,
  `MANAGEMENT=management-interface`, `LAYOUT=update`, `NETWORK=dhcp`
  verified by exact-line checks; the confirmation value is not printed.
- no `HFOC-INSTALL-SSH-KEY` line is present because no owner public key was
  supplied. The enrollment path from this page is ready: rebuild this medium
  with `HFOC_INSTALL_MEDIA_SSH_KEY` and `HFOC_INSTALL_MEDIA_SSH_KEY_CONFIRM`
  to carry the key, or use the local console fallback.
- boot entry: first entry is `/HFOC install (unattended, media-authorised)`
  with `cmdline: hfoc.install=unattended`; three entries total.
- installed configuration: `hfoc.management=management-interface
  hfoc.network=dhcp`.
- sealed media manifest and installed chain are `HFOCSIGN` envelopes.
- the token extracted from the ISO matches the staged token byte for byte.
- `scripts/hfoc-media-image-check.sh` accepted the digest and build id.

Physical Toshiba SSH acceptance remains OPEN until a fresh build-matched
OpenSSH connection and a CRC-valid remote `hfoc diag` exist. QEMU is not that
proof.
