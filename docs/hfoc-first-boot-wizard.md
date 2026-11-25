# HFOC First Boot Wizard

**Date:** 2026-09-06
**Target:** ITEM-343 of
`docs/superpowers/plans/2026-09-06-hfoc-appliance-installer-and-management-ui.md`
**Base SHA:** `d10f97a3cd923d488c5b1c79ab0956bd53eba803`
**Prerequisite boundaries:** ITEM-338..342; ITEM-303..325 configuration,
authentication, and secret protection
**Status:** Completed by ITEM-343

## Purpose

This document is the authoritative ITEM-343 first-boot wizard boundary.
ITEM-343 implements the deterministic one-shot wizard control path that
initializes the HFOC administrator credential at-rest record, hostname,
DHCP or static IPv4 configuration, gateway/DNS connectivity, and persistence
evidence before publishing the management URL.

HFOC has no production normal-boot installer loader or wizard presentation.
The module is the controller seam that the later ITEM-344 local console and
ITEM-345 web-management panel can consume. It performs no storage, network,
authentication, secret, or block-device mutation and does not claim physical
first-boot acceptance.

## Module

`installer_first_boot_wizard` lives under `kernel/` and publishes:

- a bounded one-shot stage enum;
- finalized ITEM-339 discovery evidence validation;
- administrator credential at-rest record validation;
- hostname validation through the existing ITEM-304 validator;
- DHCP/static network validation through the existing ITEM-308 network config
  boundary;
- runtime IPv4 and gateway/DNS reachability evidence;
- hostname/network/secret-record persistence commit evidence;
- exact ready-message rendering with an HTTPS management URL;
- exact status and stage strings.

The module is allocation-free and never calls a block-device write callback.

## Stage Order

The wizard stage order is:

1. `BOOT` - finalized first-boot evidence accepted.
2. `ADMIN` - administrative credential at-rest record accepted.
3. `HOSTNAME` - validated hostname accepted.
4. `NETWORK` - validated DHCP/static network accepted.
5. `CONNECTIVITY` - runtime gateway/DNS reachability accepted.
6. `PERSISTENCE` - hostname, network, and secret-record commit evidence
   accepted.
7. `READY` - ready publication is permitted.
8. `COMPLETE` - ready message published.

Every submission must occur in the exact preceding stage. Repeated,
out-of-order, and post-completion calls fail closed and leave state
unchanged.

## First Boot Evidence

The wizard requires:

- a finalized and validated ITEM-339 discovery report with at least one NIC;
- an explicit pending-first-boot evidence flag;
- an explicit storage-ready flag;
- no active recovery trust.

Recovery mode denies first-boot mutation because it must remain read-only.
Missing, incomplete, unsafe, or ambiguous discovery evidence fails closed.

## Secret Handling

The administrator stage accepts only
`APPLIANCE_SECRET_PROTECTION_KIND_ADMIN_CREDENTIAL` at-rest records. The
module validates version, kind, nonzero generation/fingerprint, and reserved
bytes, but does not retain the fingerprint or record in wizard state.

No plaintext password, raw credential, bearer token, or secret material is an
input or output of this module. Status, stage, and ready strings contain no
secret bytes.

## Persistence Evidence

The persistence stage requires caller-owned evidence that the existing
persistent boundaries committed:

- the `secret.admin.credential` fingerprint record;
- the `system.hostname` record;
- the network interface, mode, IPv4 address, prefix, gateway, and DNS
  records.

Actual journal and secret-namespace writes are owned by the existing
persistent config/secret modules and by the later integrated normal-boot
loader.

## Ready Output

The exact ready output is:

```text
HFOC Appliance is ready

Management:
https://<configured-address>/
```

The address comes from the validated runtime IPv4 config. A successful
publication marks the wizard `COMPLETE`; a too-small output buffer fails
without changing state.

## Host Proof

`make test-installer-first-boot-wizard-host` runs host fixtures that:

- complete a static IPv4 first-boot flow and verify exact ready output;
- complete a DHCP flow with a caller-supplied lease;
- reject unsafe evidence, active recovery trust, and missing first-boot
  state;
- reject malformed administrator at-rest records;
- reject invalid hostname and network configurations;
- reject unreachable gateway/DNS and static runtime mismatch;
- reject missing persistence commit evidence;
- reject repeated and out-of-order submissions;
- verify no credential fingerprint byte is rendered.

This is a host proof, not physical-machine or QEMU first-boot acceptance.

## Boundaries

- ITEM-338 owns installer architecture and storage-safety audit.
- ITEM-339 owns read-only hardware and existing-OS discovery.
- ITEM-340 owns clean installer layout.
- ITEM-341 owns dual-boot planning and boot-menu records.
- ITEM-342 owns bootloader recovery evidence and plans.
- ITEM-343 (this item) owns the first-boot wizard control boundary.
- ITEM-344 owns local boot/console presentation.
- ITEM-345 owns HTTPS management and web panel presentation.
- ITEM-346 owns integrated installer/dual-boot/management acceptance.

## Non-Goals

- No local console or web-management wizard UI.
- No production normal-boot storage loader.
- No block-device, journal, secret-namespace, network, authentication,
  session, or HTTPS mutation.
- No integrated installer acceptance.
- No QEMU proof, because this item changes no observable boot or HTTPS
  behavior.
- No physical-hardware acceptance.
- No VERSION or release-number change.
