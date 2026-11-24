# First boot and management

[Home](../README.md) · English · [Русский](operate.ru.md)

1. Complete initial owner enrollment at the local console. Sign in and check the management address using `network`.
2. Connect to the HTTPS panel at that address from the management network. Verify the server certificate fingerprint using the appliance's local output before entering credentials when using a self-signed certificate.
3. Use the panel to configure DHCP or static addressing. After an address change, reconnect to the new address. For a named HTTPS certificate, configure a reachable time source and confirm that the appliance clock is ready before importing the certificate.
4. Enroll the SSH client public key and verify the SSH host key against the local console output. Connect with `ssh hfoc@HFOC_ADDRESS`, replacing `HFOC_ADDRESS` with the address shown by `network`. SSH uses the owner key separately from the panel account.

The shell runs HFOC's native tools. Keep persistent files in `/home/owner`. Files in `/tmp` and `/run` are stored in memory.

To collect a diagnostic snapshot from another computer, run `ssh hfoc@HFOC_ADDRESS 'hfoc diag' > hfoc-diag.txt` with the pinned host key. Keep the build ID, record count and CRC in the report. When the network is unavailable, `diag save` at the local console can save a snapshot to a diagnostic medium.
