# HFOC Network Modes, Time Synchronization and Offline Bring-up

**Date:** 2026-09-11
**Target:** ITEM-374 of the HFOC Appliance Platform productization wave
**Release contract:** HFOC-APPLIANCE-V1
**Status:** Implementation and focused proof complete on the feature branch;
branch CI, Ready, merge, and post-merge gates are pending.

## Purpose

The production wave brought up DHCP, DNS, first boot, services and management,
but it had no static/offline policy, no wall clock, and no rule about which
interface may reach the management plane. TLS cannot start without a
trustworthy clock, an appliance without a network must say so instead of
pretending, and the outer interface must not expose management by default.

ITEM-374 adds exactly those three boundaries: a bounded SNTP client whose
validated sample gates TLS, an explicit offline mode with an honest `DEGRADED`
verdict, and a fail-closed management ingress binding whose default is local
only. There is no RTC in the frozen baseline, so an unsynchronized clock is
published as `UNSYNCHRONIZED`, never fabricated.

## Network Modes

`appliance_network_config` keeps `STATIC` and `DHCP` and adds an explicit
`OFFLINE` mode (`publish_offline`, `validate`, `render`, persistent
`set_offline`). `appliance_network_mode_decide` is the pure, host-testable
decision the production wire owner consumes before it touches lwIP:

| Device | Mode | Action | Addressing | Degraded |
| --- | --- | --- | --- | --- |
| present | `DHCP` | `DHCP` | yes | no |
| present | `STATIC` | `STATIC` | yes | no |
| present | `OFFLINE` | `OFFLINE` | no | yes |
| absent | any | `UNAVAILABLE` | no | yes |

The production wire owner (`appliance_production_network_wire_x86.c`) runs the
DHCP or static path exactly as before, and for `OFFLINE` it skips wire
initialization entirely and publishes
`APPLIANCE_PRODUCTION_NETWORK_WIRE_DEGRADED` with mode `OFFLINE`. Runtime state
is no longer a boot-time constant: `_boot_poll` recomputes the status, so a
DHCP failure or a stopped lifecycle publishes `DEGRADED` instead of a stale
`OK`.

The kernel prints one honest line whenever the mode or the clock verdict
changes:

```
[PRODUCTION-NET-MODE] mode=DHCP|STATIC|OFFLINE clock=READY|UNSYNCHRONIZED
```

## Time Synchronization

`appliance_time_sync` is a bounded SNTP unicast client:

- request: 48-byte SNTP client packet (`LI=0`, `VN=4`, `Mode=3`), transmit
  timestamp zero until an anchor exists, a non-zero transaction id as the
  fraction so the originate field is verifiable;
- validation: exact length, server mode 4, version 3/4, `LI != 3`, stratum
  1..15, originate equal to the request transmit, transmit epoch inside
  `[2023-11-14, 2100-01-01]`;
- anchor: `epoch_seconds(now_ms) = base_epoch + (now_ms - base_now)/1000` on
  the existing monotonic `hfoc_lwip_time` base;
- attempts: at most 3 with a 2000 ms deadline each; exhausting them publishes
  `FAILED`/`ATTEMPTS_EXHAUSTED` and, if no anchor exists, the clock is never
  ready; a resynchronization keeps the previous anchor valid until a new
  validated sample replaces it;
- refusals: every malformed, stale, unsynchronized-server and out-of-range
  reply leaves the clock unsynchronized.

`appliance_time_wire` owns the Ethernet/IPv4/UDP framing so the production
owner only resolves the next hop and transmits: the request goes to the
gateway on UDP 123 from source port 50001, and a reply is accepted only when
it is IPv4/UDP, addressed to the local address and sourced to that port.

The production wire starts the client when the link reaches `ACTIVE`, using
the gateway captured from the applied lease/static configuration. The TLS gate
is the readiness path: `appliance_production_service_boot_poll_clock` passes
`appliance_production_network_time_ready()` into
`appliance_production_service_activation_evaluate_clock`, which keeps the
service in `AWAITING_READINESS` with `MISSING_READINESS` until the clock is
synchronized. The legacy entry points keep the pre-clock behaviour for the
frozen proofs.

## Offline Bring-up

`OFFLINE` is a first-class stored mode. An offline appliance acquires no
address, sends no DHCP or DNS, keeps TLS closed because the clock stays
`UNSYNCHRONIZED`, and publishes `DEGRADED` at the wire and at boot. A missing
NIC publishes `UNAVAILABLE`. No path silently falls back from offline to DHCP
or from a failed static configuration to DHCP.

## Management Ingress Binding

`appliance_management_bind` publishes a fail-closed scope:

| Scope | Network ingress |
| --- | --- |
| `LOCAL_ONLY` (default) | all network ingress denied |
| `MANAGEMENT_INTERFACE` | only the declared interface, and only declared peers when a peer set is published |
| `OUTER_INTERFACE` | explicit opt-in |

An unpublished or malformed binding denies as `DENIED_UNCONFIGURED`; a
non-management ingress under `MANAGEMENT_INTERFACE` denies as
`DENIED_OUTER`; an undeclared peer denies as `DENIED_PEER`. Ordinary boot
publishes the default and prints:

```
[PRODUCTION-MGMT-BIND] scope=LOCAL_ONLY
```

The management listener that consumes the binding is ITEM-375; until it is
bound, the HTTPS service stays in its honest `AWAITING_*` state, so nothing is
reachable from the outer interface by construction as well as by policy.

## Evidence

| Gate | Target | Marker |
| --- | --- | --- |
| Time client and framing | `make test-appliance-time-sync-host` | `Appliance time sync host tests: ALL OK` |
| Modes, offline, binding | `make test-appliance-network-mode-host` | `Appliance network mode host tests: ALL OK` |
| Runtime DHCP + ARP + SNTP | `make test-production-dhcp-dns-qemu` | `QEMU production DHCP/DNS test: OK` and `QEMU production network/time test: OK` |

- The time proof runs 56 checks: request shape, all valid-reply fields, every
  malformed/stale/out-of-range reply, the bounded attempt budget, resynchronization,
  the wall-clock derivation and the Ethernet/IPv4/UDP framing round trip.
- The mode proof runs 24 checks: offline publication/validation/render and
  persistence, the full decision table, and every management-binding admission
  and refusal.
- The QEMU fixture now answers the guest's SNTP request in addition to DHCP and
  DNS and fails unless the guest publishes `[PRODUCTION-NET-MODE] mode=DHCP
  clock=READY`, so the runtime path is enforced by the existing DHCP/DNS proof.
- The production end-to-end acceptance stage `TIME_SYNC` names the mode proof
  and is `DEGRADED`; the doc explains that the runtime SNTP path is enforced by
  the DHCP/DNS stage and that static/offline QEMU runs and the listener binding
  are later scope.

## Explicit SNTP server (ITEM-654)

The time source can be named explicitly instead of using the active gateway.
One directive is accepted in the installed boot entry:

```
hfoc.sntp=192.0.2.7
```

The value is one literal IPv4 unicast address on the local segment.  It works
with `hfoc.network=dhcp` and with the static `hfoc.address`/`hfoc.gateway`
declaration.  The install medium seals it from the environment directive
`HFOC_INSTALL_MEDIA_SNTP=192.0.2.7` as `HFOC-INSTALL-SNTP=192.0.2.7`; the
generated installed entry then carries `hfoc.sntp=...` next to the network
tokens.

Without the directive the client keeps the pre-item behaviour: it asks the
active gateway.  ARP and UDP/123 go to the selected address, so an empty ARP
cache is resolved explicitly before the bounded retry budget is consumed.  A
reply is accepted only when its source IPv4 equals the selected server, its
destination is the active local address and its UDP ports and `originate`
match the request.  A malformed, duplicate or non-unicast `hfoc.sntp=...` is
published as `sntp=INVALID` with `clock=UNSYNCHRONIZED`; the network itself
still comes up and the source is never silently changed.  An unreachable
server exhausts the same three-attempt/2000 ms budget and leaves the clock
unsynchronized.

`[PRODUCTION-NET-MODE]` now carries the source and the client verdict:

```
[PRODUCTION-NET-MODE] mode=DHCP address=192.0.2.55/24 clock=READY
    sntp=192.0.2.7 sntp_source=EXPLICIT sntp_state=SYNCHRONIZED
    sntp_status=OK
```

`network`, the journal and `diag save` read the same serial record.  For the
Toshiba check, the already synchronized Debian host at `192.0.2.7` can serve
its own clock to the appliance segment without changing its upstream sources:

```
sudo install -m 0644 /dev/null /etc/chrony/conf.d/hfoc-sntp.conf
printf 'allow 192.0.2.0/24\n' | sudo tee /etc/chrony/conf.d/hfoc-sntp.conf
sudo systemctl restart chrony
chronyc tracking
ss -lunp | grep ':123'
```

Stop serving with `sudo rm /etc/chrony/conf.d/hfoc-sntp.conf &&
sudo systemctl restart chrony`; Debian keeps its existing synchronization.
Allow UDP/123 from `192.0.2.55` in the Debian firewall for the test window.

## Ограничения и явный сервер SNTP (ITEM-654)

Источник времени можно задать явно в установленной загрузочной записи:

```
hfoc.sntp=192.0.2.7
```

Это один локальный IPv4-адрес unicast; он работает и с `hfoc.network=dhcp`, и
со статическими `hfoc.address`/`hfoc.gateway`.  Установочный носитель
запечатывает директиву из `HFOC_INSTALL_MEDIA_SNTP=192.0.2.7` как
`HFOC-INSTALL-SNTP=192.0.2.7`, а сгенерированная запись несёт
`hfoc.sntp=...` рядом с сетевыми параметрами.  Без директивы поведение
прежнее: запрос идёт на активный шлюз.

ARP и UDP/123 идут на выбранный адрес, пустой ARP-кеш разрешается явно.
Ответ принимается только если его IPv4-источник совпадает с выбранным
сервером, адрес назначения — активный локальный, порты UDP и `originate`
совпадают с запросом.  Некорректный, повторный или не-unicast `hfoc.sntp`
публикуется как `sntp=INVALID` и `clock=UNSYNCHRONIZED`; сеть при этом
поднимается, источник молча не подменяется.  Недоступный сервер исчерпывает
прежний бюджет трёх попыток по 2000 мс и оставляет часы без синхронизации.
Строка `[PRODUCTION-NET-MODE]` показывает `sntp`, `sntp_source`,
`sntp_state` и `sntp_status`; её же читают `network`, журнал и `diag save`.

## Limitations

- No RTC and no battery-backed clock: the wall clock exists only after a
  validated SNTP sample.
- One declarative time server (the active gateway) with a fixed source port;
  DHCP option 42 and multiple servers are not parsed.
- No clock step/backwards protection beyond the epoch sanity range.
- Static and offline have no dedicated QEMU run; their proofs are host-level.
- The management binding is published and proven, but the TLS listener that
  must consult it is owned by ITEM-375.
- `VERSION` is not changed and no v1.0.0 is created.

## Non-Goals

- DHCP option 42, NTP authentication, PTP or a local RTC driver.
- Certificate lifecycle and TLS listener binding (ITEM-375).
- Any change to APIC/IOAPIC/SMP and any custom UEFI loader.
