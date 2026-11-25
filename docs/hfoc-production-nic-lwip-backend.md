# HFOC Production NIC/lwIP Backend

**Date:** 2026-09-10
**Target:** ITEM-362A of the HFOC Appliance Platform production wave
**Release contract:** HFOC-APPLIANCE-V1
**Status:** Implementation and focused host/QEMU proof complete on the feature
branch; branch CI, Ready, merge, and post-merge gates are pending.

## Purpose

This document records the ITEM-362A ordinary-boot production NIC/lwIP
backend. Before this item the real virtio-net device bring-up, lwIP allocator
and netif lifecycle, and ARP/ICMP packet path existed only inside macro-gated
QEMU proof fixtures. ITEM-362A moves that runtime path into always-compiled
production modules and binds it on the normal Limine boot.

## Implemented Boundary

Two production translation units own the boundary:

- `kernel/src/appliance_production_network.c` is the allocation-free
  composition layer. It binds one validated `network_device_snapshot`, one
  published `virtio_net_feature_profile`, and one activated
  `virtio_net_link_state` into the reused `network_device_selection`,
  `virtio_net_mac_identity`, `network_ipv4_profile`, and `hfoc_lwip_netif`
  contracts. It publishes a deterministic bounded bind result and exposes:
  - `appliance_production_network_bind()`;
  - `appliance_production_network_input()` for received Ethernet frames;
  - a transport `output()` callback seam for frames produced by lwIP.
- `kernel/src/appliance_production_network_x86.c` is the x86 production
  adapter. It performs one bounded PCI scan for vendor `0x1AF4` with the
  transitional/modern virtio-net device IDs, parses the modern capability
  chain through `virtio_pci_capability_parse()`, maps the common, notify, ISR,
  and device BARs, runs the device-status and feature negotiation, programs
  the RX/TX queues, posts RX buffers, activates the reused interrupt and link
  contracts, and calls the composition. Its transmit path copies a bounded
  frame into the TX payload region and submits it through `virtio_net_tx`.

The normal boot path calls `appliance_production_network_boot_init()` once
after the ITEM-360 production storage coordinator and polls the RX queue from
the existing kernel event loop. The bind runs on a dedicated bounded boot
stack because the frozen `virtio_net_rx_activate()` snapshots the full
available ring (64 KiB) on the stack; the ordinary boot stack must not be
overflowed by that frozen contract.

## lwIP Ownership

The production kernel links the frozen lwIP protocol source set and compiles
`kernel/src/hfoc_lwip_proto.c` with `HFOC_PRODUCTION_LWIP`. ITEM-362A reuses
the existing HFOC allocator, monotonic-time adapter, pbuf/packet ownership
bridge, netif lifecycle, and ARP/ICMP protocol boundary. It does not change the
lwIP profile, does not enable DHCP, DNS, UDP, raw sockets, or TCP services, and
does not activate entropy, mbedTLS, or HTTPS.

The default IPv4 configuration is the deterministic bounded bring-up profile
already frozen by `network_ipv4_profile_qemu_fill()` (10.0.2.15/24, gateway
10.0.2.2). Persistent desired configuration, DHCP, and DNS remain owned by
ITEM-362B and ITEM-363.

## Honest Readiness

| Condition | Readiness |
| --- | --- |
| No virtio-net function discovered | `UNAVAILABLE` |
| Device present, capability/queue/feature bind failed | `UNAVAILABLE` |
| Device bound but link not up | `DEGRADED` |
| Link up but lwIP netif/protocol path not usable | `DEGRADED` |
| Link up and published lwIP path live | `READY` |

The production boot log publishes `[PRODUCTION-NET] device=... readiness=...
status=... mac=...` without claiming activation that did not occur.

## Software Proof

| Gate | Target | Evidence |
| --- | --- | --- |
| Composition host | `make test-appliance-production-network-host` | `Appliance production network host tests: ALL OK` |
| Ordinary-boot QEMU | `make test-production-network-qemu` | `QEMU production network test: OK` |
| Full regression | `make test` | PASS (branch CI) |

The host proof binds the composition against a deterministic Ethernet sink and
checks honest `UNAVAILABLE`, MAC publication, exact ARP reply, exact ICMP echo
reply, malformed rejection, and quiescent stop. The QEMU proof boots the
unmodified production ISO with one `virtio-net-pci-non-transitional` device and
a socket backend, waits for the ordinary-boot `READY` link and MAC, and
round-trips an ARP request and an ICMP echo through the production packet path.

ITEM-388 adds the paced ring-cycle phases to that QEMU proof: 24 ARP
request/reply exchanges (ARP is not rate limited), one ICMP echo per
reachability window with every reply required, and a back-to-back burst that
asserts the documented `HFOC_LWIP_PROTO_ICMP_BUDGET` (4) /
`HFOC_LWIP_PROTO_ICMP_WINDOW_MS` (1000 ms) echo budget read from the kernel
header. The production receive ring re-posts a fresh descriptor and the
transmit ring reclaims completed frames for every one of those exchanges; a
ring that stops after the buffers posted at activation now fails the proof, and
the earlier scratch 8-of-12 signature is measured as the ICMP rate limit, not a
ring stall.

The proof paces against the guest's own clock, not wall time: a TCG guest on a
loaded host advances its monotonic milliseconds several times slower than the
host, so each paced exchange is retried across up to `REPLY_ATTEMPTS`
deadlines and the burst phase waits three spacings before it runs. The
retries never weaken the assertion — every ARP and paced ICMP exchange must
still be answered — and the burst must answer at least one and at most the
documented budget, which is exactly the "policy, not stall" contract.

Physical network hardware, a real switch/gateway, DHCP, DNS, routing, and
service/HTTPS activation remain `NOT_TESTED` or later items. No VERSION or
release-number change is made. ITEM-389 later added a second production backend
for the acceptance host's on-board Realtek NIC through the backend-neutral
`network_backend` binding and the additive
`appliance_production_network_bind_backend()` entry point; see
`docs/hfoc-production-rtl8168-backend.md`. The virtio path described here is
unchanged.

## Non-Goals

- No DHCP/DNS wire or lifecycle (ITEM-362B).
- No first-boot configuration or persistent network activation (ITEM-363).
- No service, HTTPS, or management activation (ITEM-364+).
- No physical-hardware acceptance and no VERSION/release change.
