# HFOC Administrator Guide

**Release ID:** HFOC-APPLIANCE-V1
**Scope:** Local console and HTTPS management plane

## Purpose

HFOC Appliance is a headless server appliance. Administration is primarily
performed through the HTTPS management plane. The local console provides
status, network, diagnostics, reboot, shutdown, and recovery actions where
available.

Production management activation and physical browser acceptance remain
`NOT TESTED` until a Hardware Acceptance Report records them.

## Local Console

The boot console presents:

- HFOC Appliance identity;
- hostname, CPU, memory, storage, health, network, services, and management
  state;
- local administration menu entries: Status, Network, Diagnostics, Reboot,
  Shutdown, Recovery.

No desktop environment is provided.

To set the appliance's own address from the machine's keyboard: wait for the
offer (or type `menu`), press ENTER to open the local menu, press `n` for
Network, then answer the four questions - mode (`dhcp` or `static`), the
address in `a.b.c.d/prefix` form, the gateway and the DNS.  Before the first
question the machine reads out what the interface has right now
(`[PRODUCTION-NET-SET] state=OFFERED current=DHCP`, or
`current=STATIC address=a.b.c.d/prefix`); each question and each record is one
IRQ-safe line, so a keyboard interrupt cannot split the line you are answering.
ENTER stores the answer, an empty answer cancels, and the committed record is
`[PRODUCTION-NET-SET] mode=… address=… persisted=OK applies=reboot
current=…`: the mode and addresses just stored, and the configuration the
interface keeps until the next boot.  A change therefore takes effect on
reboot; the record never claims the running address changed.  A refused answer
names its reason (`state=REFUSED reason=…`) and stores nothing, and a machine
whose state volume is not usable says `state=UNAVAILABLE
reason=NO_RECORD_OWNER` at the entry instead of asking four questions it cannot
store.  The offer record names the keys it answers to
(`[LOCAL-MENU] offered reason=IDLE entries=6 keys=s,n,d,r,p,y`).

## HTTPS Management

The management URL is:

```text
https://<configured-address>/
```

Expected sections:

- Dashboard;
- Network;
- Services;
- Storage;
- System;
- Security;
- Diagnostics;
- Updates;
- Recovery;
- Routing;
- Account.

Management uses HTTPS, authentication, and authorization. Dangerous
operations require explicit confirmation. Secrets and passwords must not
appear in ordinary application logs.

Open the panel over the network and use the menu at the top of the page: every
menu entry names a page the machine serves, so each one opens its own section
(the panel answers the sign-in document first when no session is presented).
A menu that answers `404 Not Found` is a defect, not an expected state, and the
refusal document names the target's status and reason. The link set is proven
against the served targets by the host proof and by a QEMU proof that walks
every published link as a remote client.

The Network section configures the appliance's own address. It shows two
readings side by side: the **active** configuration the interface runs now and
the **saved** configuration the next boot will apply. A change is staged,
confirmed with the single-use token the page shows, and then stored in
HFOC OS state; it applies at the next reboot, so the active reading stays what
it was until then. The console menu's network entry writes the same record, so
the panel and the machine's own menu can never disagree.

The System section configures the machine's own hostname the same way. It shows
the identity the running system serves (the name in its certificate) and the
saved hostname the next boot will apply; a change is staged, confirmed with the
token, and stored in HFOC OS state, and it applies at the next reboot. The
running listener keeps the name it booted with until then.

The Routing section (from ITEM-504) configures the facade's route table, and
the Account section (ITEM-506 increment 6) changes the one account's login and
password. The Network, System, Routing and Account sections are the configuring
ones; the remaining sections are read-only and say so.

The Account section shows the stored login and a four-field form (current
password, new login, new password twice). A change is staged and confirmed with
the token, and the commit writes the new PBKDF2 record and **signs out every
session, including the one that confirmed the change**: the browser lands on
the sign-in page on its next request and signs in again with the new pair. The
page never renders a password, and the account can also be changed on the
machine console with the `credentials` command. If the current password is
lost, the console's `credentials reset` gesture remains the recovery path (see
`docs/hfoc-production-admin-enrollment.md`).

## Operational Checks

Administrators should verify:

- appliance boot and status;
- network link and IPv4 configuration;
- DNS and gateway;
- HTTPS reachability;
- service status;
- storage health;
- persistent configuration across reboot;
- recovery path availability.

## Acceptance

HTTPS management API/web-shell QEMU proofs cover routes, TLS, failure
isolation, and restart behavior. They are not physical production browser
acceptance.

## Non-Goals

- No SSH requirement for basic operation.
- No desktop environment.
- No plaintext secret handling.
- No physical acceptance claim.
- No release version change.
