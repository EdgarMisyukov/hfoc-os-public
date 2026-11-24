# First boot and management

[Home](../README.md) · English · [Русский](operate.ru.md)

1. If the installer left the owner account unconfigured, finish enrollment at the local console using `credentials`. Sign in and check the management address using `network`.
2. Connect to the HTTPS panel at that address from the management network. When using a self-signed certificate, follow the [trusted first connection procedure](management-detail.md) before entering credentials.
3. Use the panel to configure DHCP or static addressing. Save the change, reboot the appliance and reconnect to the updated address. For a named HTTPS certificate, configure a reachable time source and confirm that the appliance clock is ready before importing the certificate.
4. Enroll the SSH client public key. Use `ssh-key show` at the local console to verify the server key before adding it to your known hosts file. SSH uses the owner key separately from the panel account.

The shell runs HFOC's native tools. Keep persistent files in `/home/owner`. Files in `/tmp` and `/run` are stored in memory.

To collect a diagnostic snapshot, substitute the management address for `HFOC_ADDRESS`. Point `-i` at the owner's private key on the client, and set `UserKnownHostsFile` to the file holding the verified server key.

```sh
ssh -i ~/.ssh/hfoc_owner -o IdentitiesOnly=yes \
  -o UserKnownHostsFile="$HOME/.ssh/hfoc_known_hosts" \
  -o StrictHostKeyChecking=yes hfoc@HFOC_ADDRESS 'hfoc diag' > hfoc-diag.txt
```

Keep the build ID, record count and CRC in a diagnostic report. When the network is unavailable, `diag save` at the local console can save a snapshot to a diagnostic medium.

- [Management, certificate setup, routing and recovery](management-detail.md)
- [Native shell reference](shell.md)
