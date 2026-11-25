# ITEM-601 SSH QEMU test fixture

`id_ecdsa` / `id_ecdsa.pub` are a **clearly test-only** ECDSA P-256 key pair.
They exist so the QEMU fixture kernel can authorize one client without an
owner key or an installed state volume.  They are never an owner credential,
never enter the production ISO/ELF, and are copied to a temporary directory and
made mode `0600` by `scripts/test-qemu-ssh.sh`.  The host key in that proof is
generated in memory from the QEMU entropy source and printed only as a public
key for `known_hosts`; no private host key is stored in the fixture.

Do not reuse these files for any real appliance.
