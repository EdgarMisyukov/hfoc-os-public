# HFOC Appliance — management reachability policy

Status: authoritative for ITEM-385. The management plane is a perimeter, and
this document states who may reach it, from where, and what happens when the
appliance cannot tell.

## The question the policy answers

Every management request, local or remote, passes one admission question before
parsing, routing, authorization or throttling:

    is this peer allowed to reach the management plane at all?

Reachability is not authentication and not authorization. It is the outer gate:
a request that fails reachability never reaches the authorization matrix, so a
denied peer learns nothing about which routes, roles or capabilities exist.
`appliance_management_reachability.h` owns the decision and
`appliance_management_reachability.c` implements it allocation-free.

## Modes

| Mode | Wire result |
|---|---|
| `CONSOLE_ONLY` | no network management at all; every network peer is refused; the local console keeps working |
| `MANAGEMENT_INTERFACE` (default intent) | the declared management interface is admitted — and, when a management address is declared, only that address on it; every other interface is refused |
| `REMOTE_OPT_IN` | as above, plus the peers in the source allowlist; when the policy requires a client certificate, an allowlisted peer without a verified certificate is still refused |

The console is evaluated first and is always allowed. It is the break-glass
path: it works with no network, with no published policy, and before the first
management interface exists. A policy therefore cannot lock an operator out of
the appliance, and it cannot be used as a denial-of-service against local
administration.

A management address of all zeroes means "any address on the management
interface". A wildcard is a deliberate operator choice, not a default: the
policy validator rejects a wildcard inside the source allowlist, because a
wildcard allowlist entry is indistinguishable from "expose the panel to
everything".

## Remote administration

Two supported ways to administer a remote appliance, in order of preference:

1. **Tunnel-only.** Terminate a VPN, an SSH port forward or a jump host on the
   management interface. The listener never appears on the outer perimeter,
   `MANAGEMENT_INTERFACE` mode is enough, and the appliance keeps a single
   declared management path. This is the recommended deployment.
2. **Explicit opt-in.** `REMOTE_OPT_IN` with a bounded source allowlist
   (`APPLIANCE_MANAGEMENT_REACHABILITY_ALLOWLIST_MAX`, currently 4 entries) and
   `require_client_certificate` set. The certificate is verified by the
   management TLS composition (ITEM-375); the reachability policy only sees
   whether a verified client certificate is present, and refuses when the
   policy demands one and it is missing.

Non-goals: exposing the panel to the internet by default, third-party SSO, and
any additional authentication factor beyond certificates. Each would be its own
item.

## Relation to the network-side bind policy

`appliance_management_bind` (ITEM-374) owns the network half: it names the
management interface and decides whether a peer may reach the appliance at all.
This policy owns the perimeter half and composes before it. One divergence is
deliberate and is recorded here so that neither reading is a surprise:

| | empty source list means |
|---|---|
| `appliance_management_bind` | any peer on the management interface — the interface binding is the control |
| this policy, `REMOTE_OPT_IN` | refused: the mode exists to admit peers that are *off* the management interface, where only an explicit list is safe |

An opt-in policy with no explicit entry is therefore invalid rather than
guessed, and the validator returns `BAD_ALLOWLIST`:

## Readiness stays honest

| Readiness | Meaning |
|---|---|
| `UNAVAILABLE` | no policy is published: no network peer is admitted, the console still works |
| `DEGRADED` | a policy is published but the appliance cannot honour it as declared: `CONSOLE_ONLY`, or a policy with no bound management interface |
| `READY` | a policy is published with a bound management interface |

A degraded appliance says so instead of pretending. The panel and any
diagnostics surface must render the readiness value rather than hiding the
section.

## Audit and break-glass

- The admission decision is the outermost point of the management path, so it
  is the correct place to record "who reached the management plane, from where,
  when" for the session audit owned by the audit and support items (ITEM-377,
  ITEM-378). This item publishes the verdict; the exportable session record
  remains owned by those items and is not duplicated here.
- Break-glass procedure: attach to the local console, read the readiness and
  mode, correct the policy, and let the appliance re-publish it. Console use is
  allowed by the policy itself, so break-glass does not require weakening any
  network rule, and it is auditable through the same console audit path.

## Proof

`tests/appliance-management-reachability-host.c` proves the decision table:
validation, first-publication-only, readiness for every state, console
admission with no policy, the interface boundary, the address boundary, the
allowlist, the certificate requirement, and the argument guards.

`kernel/src/appliance_management_reachability_qemu.c` proves the same table
inside a real QEMU boot (`[MGMT-REACH]` markers), and
`scripts/test-qemu-management-plane-acceptance.sh` proves over a real TLS 1.3
connection that the live wire path consults the policy:

- a peer is admitted while the appliance serves the management interface, and
  the acceptance flow asserts the served response;
- after the appliance publishes `CONSOLE_ONLY`, the same live peer is refused on
  the wire with the unknown-route response, so the refusal does not confirm that
  a management plane exists;
- when the appliance serves management again, the served response returns.

Two honest limitations are recorded here rather than hidden:

1. The appliance has a single network interface today (`HFOC_LWIP_NETIF_NUM` is
   a single `et0`, and `netif_add()` is contractually forbidden in the netif
   composition by `scripts/check.sh`). The interface dimension of the policy is
   therefore carried as a declared identifier, proved by the host and QEMU
   policy proofs, while the wire proof exercises the address dimension end to
   end with the peer address read from the accepted TLS connection. A
   two-interface wire proof needs multi-netif registration, which is its own
   item: with a single network interface this proves the decision and its
   wiring, not the frame topology.
2. The live management request path that exists in production today is the
   production management composition; the fixture kernel that serves management
   over the wire is the QEMU acceptance image, exactly as for the rest of the
   management wave. Extending the reachability gate to a production listener is
   the same prerequisite that the panel and the management API share.

The policy decides *who* may reach the panel; it assumes the appliance has an
address to be reached on. Since ITEM-408 that assumption holds through a
transient network failure: a renewal, rebind or DNS-probe timeout no longer
deactivates the live netif or clears the applied lease, so a peer that was
admitted before the failure is still admitted after it and the appliance keeps
the `MANAGEMENT: https://<address>/` URL it published. Only an explicit DHCP
refusal or a link that stayed down takes the address away, and the
`[PRODUCTION-DHCP] reason=` field names which of those it was. The policy
itself, its default and its refusals are unchanged.
