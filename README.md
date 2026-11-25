# HFOC OS

English · [Русский](README.ru.md) · [Official site](https://hfoc.ru/)

HFOC OS is an x86-64 operating system for network appliances. Version 1.0.0 provides a web management panel in English and Russian, SSH with an owner key, a native shell, and a persistent workspace backed by HFOCFS.

- [Release and hardware support](docs/release-1.0.0.md)
- [Boot and installation options](docs/install.md)
- [Configuration, SSH and diagnostics](docs/operate.md)
- [Building the boot image](docs/build.md)
- [Licensing and contributing](docs/licensing.md)

Download the generic BIOS/UEFI boot ISO, checksums and SPDX inventory from [release v1.0.0](https://github.com/EdgarMisyukov/hfoc-os-public/releases/tag/v1.0.0). This repository contains the public source snapshot and the release guides. The [build guide](docs/build.md) explains the source revision and verification commands.

The release was built from `933f7cd739a9437e86638fbfe30a738e22f2e9aa`. The verified Toshiba L875 build reports `933f7cd739a9`.

HFOC-owned code and documentation are offered under [GNU AGPL v3 only](LICENSE) or a separate written commercial license. Bundled components retain their upstream licenses.

Project contact [mis@hfoc.ru](mailto:mis@hfoc.ru).

<details>
<summary>Release check evidence / Материалы проверок выпуска</summary>

The following excerpts support release verification.
Ниже приведены выдержки для проверки выпуска.

```text
## Production NIC/lwIP backend
ITEM-391 declares the installed footprint of the appliance instead of letting
each, and a target disk below the exact minimum media (331810 sectors, >= 162
ITEM-397 stages the installed system into the ESP and makes the disk boot: the
[`docs/hfoc-appliance-management-authentication-architecture.md`](docs/hfoc-appliance-management-authentication-architecture.md).
[`docs/hfoc-appliance-administrative-authentication.md`](docs/hfoc-appliance-administrative-authentication.md).
[`docs/hfoc-appliance-management-authorization.md`](docs/hfoc-appliance-management-authorization.md).
[`docs/hfoc-appliance-secret-protection.md`](docs/hfoc-appliance-secret-protection.md).
[`docs/hfoc-appliance-persistent-storage-namespaces.md`](docs/hfoc-appliance-persistent-storage-namespaces.md).
[`docs/hfoc-appliance-configuration-integrity.md`](docs/hfoc-appliance-configuration-integrity.md).
[`docs/hfoc-appliance-certificate-secret-persistence.md`](docs/hfoc-appliance-certificate-secret-persistence.md).
[`docs/hfoc-appliance-health-model.md`](docs/hfoc-appliance-health-model.md).
[`docs/hfoc-appliance-runtime-diagnostics-logs.md`](docs/hfoc-appliance-runtime-diagnostics-logs.md).
[`docs/hfoc-appliance-https-health-operational-status.md`](docs/hfoc-appliance-https-health-operational-status.md).
[`docs/hfoc-appliance-update-package-architecture.md`](docs/hfoc-appliance-update-package-architecture.md)
[`docs/hfoc-appliance-update-manifest-validation.md`](docs/hfoc-appliance-update-manifest-validation.md).
## Update integrity verification
[`docs/hfoc-appliance-update-integrity-verification.md`](docs/hfoc-appliance-update-integrity-verification.md).
## Appliance recovery mode
[`docs/hfoc-appliance-recovery-mode.md`](docs/hfoc-appliance-recovery-mode.md).
## Appliance factory reset
[`docs/hfoc-appliance-factory-reset.md`](docs/hfoc-appliance-factory-reset.md).
## Integrated appliance boot acceptance
[`docs/hfoc-appliance-integrated-boot-acceptance.md`](docs/hfoc-appliance-integrated-boot-acceptance.md).
## Persistent configuration reboot acceptance
[`docs/hfoc-appliance-persistent-config-reboot-acceptance.md`](docs/hfoc-appliance-persistent-config-reboot-acceptance.md).
## Management plane QEMU acceptance
## HFOC Appliance Platform release acceptance
[`docs/hfoc-appliance-platform-release-acceptance.md`](docs/hfoc-appliance-platform-release-acceptance.md).
## UEFI installer helper
## Installer architecture audit
## Hardware and existing OS discovery
## Clean installer layout
## Bootloader recovery evidence
## First boot wizard
## Appliance boot console
## Management web UI
No browser JavaScript is introduced. The remote credential form is exactly one
## Management reachability policy
## Integrated installer, dual-boot and management acceptance
## Release documentation
- Stage 2 IOAPIC IRQ0 shadow route: PIT/IRQ0 дополнительно программируется на
  vector `0x40`; vector `0x40` — shadow route, не владеет ticks;
  authoritative owner времени остаётся legacy PIC vector 0x20;
  PIC controllers to FF/FF; missing precondition or transaction failure retains
  the existing fallback policy; logical mask disable, not IMCR/hardware removal;
[PIC ] Master mask: 0x00000000000000FC; slave mask: 0x00000000000000FF
## Security domain and syscall origin
## Capability-gated syscall operations
## Per-process CPU tick and user-memory limits
## Bounded kernel event ring
## Structured audit records
## Consistent audit/event snapshots
## Appliance audit observability
## Strict appliance configuration schema
## Transactional appliance configuration snapshots
## Static system image manifest
## SHA-256 measurement
## Pre-publication image integrity
## Verified recovery-image selection
## Boot-policy QEMU regression
## Bounded block-device abstraction
## Fixed appliance data-region discovery
## Fixed-size block cache ownership
make test-config-journal
## Bounded configuration/audit checkpoint KV API
## State schema registry
## State migration transaction
## Journal compaction commit
## Deterministic backup stream
`backup_stream_frame_validate()` is the framing v1 pre-restore gate. It
## Backup manifest inventory
## Bounded backup producer
## Restore validation and no-change rollback
## Restore dry-run planner
## Staged restore activation
## Backup/restore QEMU acceptance
## Service dependency descriptors
## Dependency graph snapshots
## Ordered service start
## Ordered service stop
## Restart policy descriptor
## Service restart containment
## Service resource envelope
## Service readiness deadline
## Service state snapshot
liveness generation, and readiness deadline fields. Entries are sorted by
## Supervisor composition QEMU proof
## Local principal descriptor
## Credential verifier contract
## Local session lifecycle
## Failed-authentication throttle
## Management command schema
## Authorization matrix
## Read-only appliance status command
## Controlled configuration command
## Management audit correlation
## Local-control QEMU proof
## Network device descriptor
## Link-state model
## Virtio PCI identity contract
## Virtio PCI capability parser
## Virtio device status lifecycle
## Virtio PCI feature negotiation
## Virtqueue layout calculator
## Virtqueue descriptor ownership
## Virtqueue available publication
## Virtqueue used consumption
## Virtio DMA region contract
## Virtio foundation QEMU proof
## Virtio-net feature profile
## Virtio-rng feature profile
## Virtio-rng queue composition
## Virtio-rng random request API
## Virtio-rng completion path
## Entropy continuous health
## Entropy accumulator lifecycle
## TLS seed/fill contract
## Entropy diagnostics privacy
## Entropy failure and restart
## Entropy QEMU proof
## Virtio-net queue composition
## Virtio-net RX completion
## Virtio-net TX reclaim
## Virtio-net interrupt path
## Virtio-net link-state binding
## Virtio-net reset and shutdown
## Virtio-net frame QEMU proof
## Fixed IPv4 configuration parser
## Network configuration transaction
## Bounded packet buffer ownership
## Network packet-pool descriptor
## Packet acquire/release lifecycle
## Packet slice and cursor API
## Ethernet frame validator
## MAC identity publication
## Static QEMU IPv4 profile binding
## HFOC netif input contract
## HFOC netif output contract
## Packet pressure metrics and audit
## Packet-boundary QEMU proof
## mbedTLS source and license lock
## Minimal mbedTLS TLS 1.3 configuration
CTR-DRBG.  ITEM-508 adds the client role the ACME module needs (profile
## mbedTLS allocator boundary
## mbedTLS entropy/DRBG adapter
## mbedTLS certificate/key manifest parser
## mbedTLS test-secret build separation
## mbedTLS BIO transport adapter
## TLS handshake state machine
## TLS close and error normalization
## TLS 1.3 QEMU proof
## HTTP parser state descriptor
## Incremental request-line parser
## Incremental header parser
## HTTP framing and body rejection
## HTTP method/version and Host policy
## HTTP fixed route table
## HTTP deterministic response encoder
## HTTP HEAD response semantics
## HTTP error matrix
## HTTP parser/encoder host acceptance
## HTTPS service descriptor
## HTTPS admission transaction
## HTTPS connection composition
## HTTPS TLS-to-HTTP data flow
## HTTPS root and health handlers
## HTTPS health/audit/metrics
## HTTPS fifth-connection denial
## HTTPS peer-failure throttle
## HTTPS graceful stop and exact restart
## HTTPS service QEMU proof
## Standard HTTPS client harness
## Exact HTTPS successful response matrix
## Encrypted HTTPS malformed-request matrix
## Encrypted HTTPS size-boundary matrix
## Concurrent HTTPS exhaustion matrix
## Entropy fail-closed matrix
## Certificate/key fail-closed matrix
## Production artifact secret gate
## HTTPS restart/soak rehearsal
## HTTPS vertical-slice acceptance gate
## lwIP pbuf/packet ownership bridge
## lwIP netif lifecycle
make test-arp-icmp-qemu
## Raw TCP listener contract
## TCP connection lifecycle adapter
## Four-slot connection pool
## Listener backlog admission
## TX backpressure propagation
## Stale callback quarantine
## Malformed TCP isolation matrix
## Peer close and local abort policy
## Listener drain and restart
## TCP listener QEMU proof
## Network resource QEMU stress
## Minimal ICMP reachability
## Management transport framing
## Network admission policy
## Network audit/health events
## Trust-policy QEMU proof
## Verification policy snapshot
## Key use separation
## Signed metadata envelope
## Trust anchor rotation staging
## Trust anchor revocation
## Image-policy binding
## Measured boot evidence record
## Recovery trust mode
## Update compatibility manifest
## Candidate slot inventory
## Candidate preflight
## Candidate staged write verification
## Activation intent record
## First-boot health contract
## Automatic rollback selection
## Update acceptance commit
## Update retention policy
## Update lifecycle QEMU proof
## Health-check descriptor
## Storage health check
## Service health aggregation
## Network health aggregation
## Audit-ring pressure health
## Versioned metrics snapshot
`support_bundle_exporter.c` adds the Wave Q support-bundle exporter boundary.
A fixed `HFOCSBEX` stream emits the support-bundle manifest and the six
## Diagnostic privacy policy
## Supportability QEMU proof
## External HFOC accounting integration boundary
## External integration principal scope
## External integration session handshake
## Accounting hand-off envelope v1
## Idempotency ledger
## Outbound event cursor
## Inbound configuration contract
## Integration failure isolation
## Integration compatibility policy
## Accounting-boundary QEMU proof
## Fault-injection catalogue
## Storage interruption drill
## Service cascade drill
## Local-control outage drill
## Network partition drill
## Trust failure drill
## Update failure drill
## Integration replay/outage drill
## Power-cycle continuity drill
## Resilience matrix QEMU proof
## BSP service supervisor lifecycle
PS/2 Set 1 decoder and console line editing: US Set 1, Shift/CapsLock, E0
ANSI. IRQ1 vector `0x41` ownership unchanged. The fixed 16-entry command history
uses non-empty newest-only deduplication and exact draft restore. No persistence, completion/search, ANSI, UTF-8, keypad, or heap.
## Проверка IOAPIC IRQ1 routing и Stage 3a PIC IRQ1 masking
make test-ioapic-irq1
- exact success marker `[PIC ] IRQ1 masked; IOAPIC vector=0x41 active.`;
  `[PIC ] Master mask: 0x00000000000000FE; slave mask: 0x00000000000000FF`;
  `Servicing hardware INT=0x41`; Stage 3a IRQ1 remains independent;
Stage 3b PIC IRQ0 masking реализован в `main`. Настоящий QEMU-тест на portable
- verified IOAPIC route makes vector 0x40 authoritative PIT timer owner:
- PIC IRQ0 is masked only in success: success branch вызывает
- fallback preserves PIC vector 0x20 timer ownership:
- `0x20` after success is stray PIC EOI only: ровно `pic_send_eoi(0)`,
  `[PIC ] Master mask: 0x00000000000000FF; slave mask: 0x00000000000000FF`;
make test-pic-irq0-masking-fallback
- legacy `max,-apic` `0xFC/0xFF` remains separate:
## Проверка one-shot LAPIC timer probe
  `[LAPIC-TIMER] oneshot armed: vector=0x43` и
- `TICKS` растут после восстановления periodic 0x42;
make test-ioapic-irq0
ISA IRQ8–15 use verified IOAPIC vectors 0x49–0x50; PIC IRQ8–15 lines are masked only after route publication and remain fallback otherwise.
Portable QEMU xAPIC baseline: q35, one vCPU, QEMU default xAPIC-capable CPU; -cpu max,-apic is not required.
## Release profile descriptor
## Reproducible build input manifest
## Reproducible image comparison
## Release artifact manifest
## Release upgrade matrix
## Release backup/restore matrix
## Release security baseline
## Release operational baseline
## Appliance v1 disaster-recovery rehearsal
## Appliance v1 acceptance gate
## Production end-to-end acceptance
## Product release gate (ITEM-380)
## Hardware acceptance (ITEM-381)
## Signed media and installer hardening (ITEM-371)
## Signed boot chain and boot attestation (ITEM-372)
## Storage encryption and key custody (ITEM-373)
## Network modes, time synchronization and offline bring-up (ITEM-374)
## Support bundle, redaction and break-glass (ITEM-378)
## Trademark and certified build policy (ITEM-383)
- Панель управления отдаёт страницу `/panel/about/licenses`: состояние
```

</details>
