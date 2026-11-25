# HFOC First Boot Guide

**Release ID:** HFOC-APPLIANCE-V1
**Scope:** Initial appliance administrator, hostname, and network setup

## Purpose

After HFOC is installed, the first boot provides one-shot configuration for
the administrator credential, hostname, network mode, IPv4 settings, DNS,
connectivity, persistence, and reboot. Physical first-boot configuration
remains `NOT TESTED` until a Hardware Acceptance Report records it.

## Required Inputs

- administrator credential at-rest record;
- hostname;
- NIC;
- DHCP or static IPv4 mode;
- IP address and prefix for static mode;
- gateway and DNS;
- connectivity evidence;
- persistence commit evidence.

## Administrator Password

The administrator password must never be printed to logs or displayed on the
console. HFOC boundaries accept only an opaque at-rest administrator
credential record and never render raw password bytes.

## Network

Use DHCP when a server is available. For static configuration:

- IP address;
- prefix or netmask;
- gateway;
- DNS.

The wizard must validate runtime gateway/DNS reachability before publishing
the appliance as ready.

## Persistence And Reboot

The first-boot configuration must be committed through the persistent
configuration and secret boundaries before reboot. On reboot, the appliance
should load the committed hostname and network state or fail safe with a
recovery path.

## Ready Output

After configuration is committed, the appliance should show:

```text
HFOC Appliance is ready

Management:
https://<configured-address>/
```

## Acceptance

First-boot wizard host proofs validate stage order, credential records,
hostname, network, connectivity, persistence, and ready output. They are
not physical first-boot acceptance.

## Non-Goals

- No plaintext password handling.
- No destructive action without confirmation.
- No physical acceptance claim.
- No release version change.
