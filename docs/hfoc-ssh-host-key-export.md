# SSH host key on the diagnostic medium

ITEM-613 removes the photograph/manual-base64 step for the appliance's SSH host
identity.  The normal diagnostic snapshot now carries the **public** OpenSSH
host key, and the Debian reader writes the files an operator needs.

## What is exported

One additional diagnostic record, produced by the same snapshot composer as
`diag save` and the SSH `hfoc diag` stream:

```
[PRODUCTION-SSH-HOSTKEY] v=1 state=READY
  algorithm=ecdsa-sha2-nistp256
  blob=<base64 OpenSSH public blob>
  fingerprint=SHA256:<base64 SHA-256, no padding>
  address=<current IPv4 or none> port=<22 or the actual port>
  build=<build id>
```

Only the public blob and its fingerprint leave the machine.  The private
scalar, the authorized client key, session tokens, the seed, passwords and
KDF verifiers are never part of the record.  An unavailable identity is
`state=UNAVAILABLE reason=...`, never an empty or invented key.  The long
record is split by the existing `[DIAG+]` continuation rule and the reader
reassembles it; the session BEGIN/END/count/CRC checks are unchanged.

## Operator path

1. On the appliance: `diag save` (the normal diagnostic medium must be
   attached).
2. Power off, move the **diagnostic** medium to Debian.
3. On Debian:

   ```
   sudo python3 scripts/hfoc-diag-read.py --output ~/hfoc-diag
   ```

   The reader verifies the session CRC/count, then writes:

   ```
   ~/hfoc-diag/HFOC-DIAG-<session>.txt
   ~/hfoc-diag/HFOC-SSH-HOST-<session>.pub
   ~/hfoc-diag/HFOC-SSH-HOST-<session>.fingerprint
   ~/hfoc-diag/HFOC-SSH-HOST-<session>.known_hosts.candidate
   ```

4. Check the key independently:

   ```
   ssh-keygen -lf ~/hfoc-diag/HFOC-SSH-HOST-<session>.pub
   cat ~/hfoc-diag/HFOC-SSH-HOST-<session>.fingerprint
   ```

   Both must name the same `SHA256:...` value.  The reader recomputes it from
   the blob itself and refuses a malformed/truncated blob, a wrong curve or
   point, a mismatched declared fingerprint or a broken session CRC.

5. Review the candidate, then install it deliberately (the reader writes a
   candidate; it does not edit `~/.ssh`):

   ```
   cat ~/hfoc-diag/HFOC-SSH-HOST-<session>.known_hosts.candidate \
       >> ~/.ssh/hfoc_known_hosts
   ssh -o UserKnownHostsFile=~/.ssh/hfoc_known_hosts \
       -o StrictHostKeyChecking=yes -p 22 <address> 'hfoc status'
   ```

   A non-standard port is written as `[address]:port`; the record carries the
   actual address/port, so a DHCP change or a moved port is visible instead of
   silently reusing the old pin.

## Failure and compatibility rules

- An old valid session without the key record still extracts its
  `HFOC-DIAG-<session>.txt`; the reader prints
  `hostkey=ABSENT reason=NO_HOSTKEY_RECORD` and writes no trusted key file.
- `state=UNAVAILABLE` is named with its reason and writes no key file.
- Corrupt/truncated blobs, a wrong algorithm/curve/point, a mismatched
  fingerprint, a bad port or contradicting duplicate key records are refused;
  no `.pub` or `known_hosts` candidate is written.
- An existing different pinning file is never overwritten: the reader keeps it
  and writes `...known_hosts.candidate.new` for review.
- The public files are written with mode 0600.
- QEMU is a production-path regression, not a physical Toshiba acceptance.  The
  owner's physical medium transfer remains the trust boundary.

## Implementation

- `kernel/src/appliance_production_ssh.c` renders the public record from the
  already-loaded `appliance_ssh_identity`; the private scalar is not read.
- `kernel/src/kernel.c` publishes it in `kernel_production_status_report()`, so
  both `diag save` and `hfoc diag` carry it through the one composer.
- `scripts/hfoc-ssh-hostkey.py` validates/renders the files;
  `scripts/hfoc-diag-read.py` calls it only after the session CRC/count check.
- Proofs: `tests/diag-export-full-host.c` drives the real journal and reader,
  `scripts/test-ssh-hostkey-export-host.sh` covers valid/absent/unavailable/
  corrupt/duplicate/old/DHCP/conflict cases, and `scripts/test-qemu-ssh.sh`
  requires the reassembled production stream blob to equal the handshake key
  and its independent fingerprint.
