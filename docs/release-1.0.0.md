# HFOC OS 1.0.0

[Home](../README.md) · English · [Русский](release-1.0.0.ru.md)

Version 1.0.0 is the first product release. Its native kernel targets x86-64 and boots through Limine in BIOS and UEFI configurations.

## Features

- The web panel configures network addressing, HTTPS certificates and HTTPS routes to a declared backend. The interface offers English and Russian.
- Owners can use SSH public-key authentication to open a native shell with command history, UTF-8 line editing, pipelines, redirections and job control.
- The `/home/owner` directory is persistent. A state-preserving update keeps the owner account, SSH identity and files.
- SSH diagnostics can be downloaded with record counts and CRCs for verification.

## Verification and limits

The release image was reproduced in two clean builds. Virtual-machine tests covered BIOS/UEFI and installation, SSH, shell behavior and persistence. On a Toshiba L875 with RTL8168 Ethernet, testing confirmed address assignment, web sign-in, SSH, an HTTPS route and file persistence across a reboot. The first panel sign-in took about 33 seconds in that hardware run. Brief connection resets were observed around diagnostic collection, followed by successful probes.

HFOC OS runs native programs. Linux binary compatibility and full POSIX coverage are outside this release. Support depends on the storage controller, network adapter and firmware of the target machine.

Release source `933f7cd739a9437e86638fbfe30a738e22f2e9aa`. Build `933f7cd739a9`.
