# Management and recovery for HFOC OS 1.0.0

[First boot and management](operate.md) · English · [Русский](management-detail.ru.md)

## First boot and the administrator account

1. Boot from the installed disk and keep the local console available while
   setting up the network. At the console, run `network` to see the management
   address and clock state.
2. If the installer left the account unset, run `credentials` locally. Enter
   the administrator login and the new password twice when prompted. The login
   is case-sensitive and takes 1–32 ASCII letters, digits, dots, underscores or
   hyphens. Use a unique password of 12–128 printable ASCII characters. Verify
   that setup succeeded, then use `login` for local password authentication.
3. From the management network, open `https://<management-address>/panel`.
   Establish trust in the server certificate before entering your password.
   For the appliance's self-signed certificate, compare the browser's SHA-256
   fingerprint with a value obtained over a trusted direct link as described
   below.
   A cold first sign-in can take around 33 seconds. Wait for it to finish
   before submitting the form again.
4. Open **Network**, stage either DHCP or static IPv4 settings and confirm the
   change with the token shown by the panel. After reboot, use `network` at
   the console to verify the active address before reconnecting.

To change the account from the panel, open **Account**, enter the current
password and the new login and password, then confirm with the single-use
token. The change revokes the session that confirmed it and all other panel
sessions. Sign in again with the new credentials.

For the first connection with the self-signed certificate, connect your
workstation directly to the appliance or use an isolated management segment
you control. Obtain the certificate fingerprint over that connection before
using another network. Substitute the management address shown by `network`.

```sh
HFOC_ADDRESS=192.0.2.10
openssl s_client -connect "$HFOC_ADDRESS:443" -noservername </dev/null 2>/dev/null \
  | openssl x509 -noout -sha256 -fingerprint
```

## Time and named HTTPS

The appliance queries the active gateway for time by default. To use a
different reachable SNTP server on the local segment, set
`HFOC_INSTALL_MEDIA_SNTP=192.0.2.20` when generating declared installation or
update media. The resulting installed boot command line carries
`hfoc.sntp=192.0.2.20`. Substitute your server's literal unicast IPv4 address
for the example. See [boot and installation](install.md).

After boot, run `network` and wait for `clock=READY` before importing a named
certificate. If the panel answers `BAD_CLOCK`, check the selected SNTP
address and UDP port 123 reachability from the appliance. Retry the import
after the clock is ready.

1. Configure DNS for the hostname you will use. Obtain a currently valid
   certificate with that exact name in its subject alternative names and a
   matching ECDSA P-256 private key.
2. In **Certificates** (`/panel/certificates`), enter the hostname, certificate
   PEM and matching key PEM. Stage the import, review the pending operation
   and confirm it with the displayed single-use token.
3. Reboot to load the saved certificate. Connect using the DNS name so the
   client sends the expected server name (SNI). Verify the chain and hostname
   with the client's trust store or the appropriate CA certificate.

The **Security** page (`/panel/security`) has the appliance certificate import
form with a separate optional chain field. The **Certificates** page manages
certificates for individual names.

## SSH client and appliance identity

The SSH login is `hfoc` and uses the enrolled owner's ECDSA P-256 public key.
Generate a client key on your workstation if you do not already have one.
Keep the private key on that workstation.

```sh
install -d -m 700 "$HOME/.ssh"
ssh-keygen -t ecdsa -b 256 -f "$HOME/.ssh/hfoc_owner"
ssh-keygen -lf "$HOME/.ssh/hfoc_owner.pub"
```

The declared update medium can install this key with
`HFOC_INSTALL_MEDIA_SSH_KEY` set to the `.pub` file and
`HFOC_INSTALL_MEDIA_SSH_KEY_CONFIRM` set to its `SHA256:…` fingerprint.
For local enrollment, extract the 88-character base64 public point from the
OpenSSH public key using Python 3 on your workstation.

```sh
python3 - "$HOME/.ssh/hfoc_owner.pub" <<'PY'
import base64
import struct
import sys
from pathlib import Path

key_type, encoded, *_ = Path(sys.argv[1]).read_text().split()
if key_type != "ecdsa-sha2-nistp256":
    raise SystemExit("Expected an ECDSA P-256 public key")
blob = base64.b64decode(encoded, validate=True)
fields = []
for _ in range(3):
    size, = struct.unpack(">I", blob[:4])
    fields.append(blob[4:4 + size])
    blob = blob[4 + size:]
algorithm, curve, point = fields
if (algorithm != b"ecdsa-sha2-nistp256" or curve != b"nistp256" or blob
        or len(point) != 65 or point[0] != 4):
    raise SystemExit("Invalid ECDSA P-256 public key blob")
print(base64.b64encode(point).decode("ascii"))
PY
```

At the local console after `login` or `credentials`, run
`ssh-key add <88-character-base64-point>`. Compare the pending fingerprint
with the one from `ssh-keygen -lf`, then run
`ssh-key confirm <SHA256:fingerprint>` with the exact value. Use
`ssh-key show` to verify both the enrolled client fingerprint (`authorized=`)
and the appliance host fingerprint (`host=`). To revoke the enrolled client
key, use `ssh-key remove` at the authenticated local console.

Replace the example address below with your appliance's management address.
Fetch an SSH host key candidate and compare its fingerprint with the
`host=` value read locally.

```sh
HFOC_ADDRESS=192.0.2.10
ssh-keyscan -t ecdsa "$HFOC_ADDRESS" > hfoc-host.candidate
ssh-keygen -lf hfoc-host.candidate
```

Only after the fingerprints match, save the pin in a dedicated file. Choose a
separate file for each appliance and review existing pins before replacing one.

```sh
install -m 600 hfoc-host.candidate "$HOME/.ssh/hfoc_known_hosts"
ssh -t -i "$HOME/.ssh/hfoc_owner" -o IdentitiesOnly=yes \
  -o UserKnownHostsFile="$HOME/.ssh/hfoc_known_hosts" \
  -o StrictHostKeyChecking=yes "hfoc@$HFOC_ADDRESS"
```

For diagnostics without an interactive terminal, run the same `ssh` command
without `-t` and add `'hfoc diag'` after the destination. If the network is
unavailable, use `diag save` on the local console with a prepared diagnostic
medium. See [first boot and management](operate.md) for report handling.

## HTTPS routing

In **Routing** (`/panel/routing`), stage a route with an exact DNS name, the
declared backend's IPv4 address and port, and the backend TLS setting.
Confirm the operation and check that it appears in the applied table. Use
**Remove** with the route name and confirm to delete it.

The HTTPS facade terminates client TLS and sends a plain HTTP request to the
backend for `tls=off`. Use this on a trusted backend network. A route with
`tls=on` is refused with HTTP 503 when requested. The firewall and backend
should allow only the intended traffic. `/panel` and `/api/` paths remain
management endpoints.

For example, point `app.example.org` at the appliance and import a certificate
for that name. With a route to the intended backend and a trusted CA, test
the route from a management workstation. Substitute your own hostname,
address, CA file and a path that your backend serves.

```sh
curl --fail --cacert management-ca.pem \
  --resolve app.example.org:443:192.0.2.10 https://app.example.org/facade
```

Check the response against the backend's log. If you configured a separate
declared TCP forward on port 9443, test that endpoint with its backend
hostname and port 9443. In that mode the backend presents the certificate
and only explicitly listed client IPv4 addresses can connect.

## Recover access

- **Forgotten administrator password.** At the local console, enter the
  following two commands consecutively. Any intervening command disarms the
  confirmation.

  ```text
  credentials reset
  credentials reset confirm
  ```

  Wait for `state=UNPROVISIONED`, run `credentials` to set a new account and
  enroll the SSH client key again. The reset revokes existing sessions and the
  authorized SSH client key. If the console reports a storage failure,
  address it before relying on a reset or revocation across reboot.
- **Network or certificate access lost.** Use the local console to check
  `network`, `ssh-key show` and the saved settings. Restore the trusted
  management network and verify the server certificate or SSH host key before
  reconnecting.
- **Boot recovery or factory reset.** Back up `/home/owner` and record the
  current boot entries, especially on a dual-boot system. Use the appliance
  recovery path to inspect state before authorizing writes. A factory reset
  erases persistent appliance settings and secrets and requires a reviewed
  plan and explicit confirmation. For a boot repair, retain the shared EFI
  System Partition and unrelated UEFI entries, then verify every preserved OS
  still boots. See [boot and installation](install.md).
