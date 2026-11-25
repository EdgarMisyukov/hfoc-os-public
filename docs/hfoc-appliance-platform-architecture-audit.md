# HFOC Appliance Platform Architecture Audit

**Date:** 2026-09-03
**Target:** ITEM-301 of `docs/superpowers/plans/2026-09-03-hfoc-appliance-platform.md`
**Base SHA:** `e20a623c8c39448fabc9bd6ba2c8696c888fc582`
**Status:** Completed by ITEM-301 in PR #322

## Audit Scope

This audit examines the repository state on synchronized `main` after items
1-300. It classifies what is already reusable for a self-contained HFOC
Appliance Platform, what is only proven in macro-gated host/QEMU fixtures,
and which capabilities must still be composed into a production boot path.

The source of truth is the actual code, Makefile targets, scripts, QEMU
proofs, acceptance gates, and documentation. No claim below should be read as
an assertion that test-only proof behavior exists in the normal production
image unless the normal boot path is named.

## Boot Sequence

`kernel/src/kernel.c` owns the normal Limine boot path:

- validate Limine base revision, framebuffer, memory map, and HHDM;
- initialize PMM, VMM, heap, event ring, ACPI/MADT snapshot, and BSP state;
- initialize GDT/TSS/IDT, Local APIC/IOAPIC/PIC, keyboard, PIT, and LAPIC
  timer contracts;
- initialize console, scheduler-related test paths, and enter the HLT event
  loop.

The production boot path initializes `hfoc_lwip_time_init_default()` but does
not activate persistent appliance state, a production network lifecycle, a
service supervisor, the HTTPS service, or a management plane. Those
capabilities are implemented as bounded modules and are exercised by
dedicated proof builds such as `HFOC_CONFIG_JOURNAL_QEMU_TEST`,
`HFOC_SERVICE_SUPERVISOR_QEMU_TEST`, `HFOC_HTTPS_SERVICE_QEMU_TEST`, and the
acceptance gates.

## Kernel, Memory, Interrupts And Execution

Existing reusable primitives:

- freestanding x86-64 kernel with GDT, IDT, TSS, IST, panic, serial, and
  framebuffer console;
- legacy PIC, verified IOAPIC routes, Local APIC, LAPIC timer vector `0x42`,
  and BSP/SMP ownership policy;
- PMM, VMM, address spaces, heap, per-CPU state, and macro-gated hardware
  proofs;
- scheduler, kernel processes, PCB/PID ownership, descriptors, capabilities,
  CPU/user-memory limits, user address-space layout, Ring 3 entry, and the
  syscall/sysret ABI;
- event ring, structured audit, snapshots, and no-allocation interrupt
  contracts.

The execution model remains BSP-owned for vector `0x40`-`0x50`, PIC, IOAPIC,
and periodic LAPIC timer `0x42`. The Appliance Platform queue should preserve
these contracts unless a focused future item explicitly changes one.

## Storage And State

Existing reusable primitives:

- bounded block-device abstraction and fault model;
- fixed appliance data-region discovery;
- fixed-size block cache;
- checksummed append-only configuration journal with atomic commit recovery;
- bounded config/audit checkpoint KV API;
- state schema registry and migration transaction;
- journal compaction with active-generation preservation;
- deterministic backup stream, manifest, producer, restore planner, staged
  restore, and QEMU acceptance;
- strict appliance configuration schema and transactional snapshots.

There is deliberately no general writable filesystem. The platform must keep
this boundary. The current modules are caller-owned host/state machines and
QEMU proof components; there is no production persistent state loader that
discovers a device, validates a namespace, applies safe defaults, publishes
configuration to runtime services, and records the active generation during
normal boot.

## Console, Serial And Local Control

Existing reusable primitives:

- PS/2 keyboard line editing and deterministic console history;
- COM1 serial logging and QEMU monitor-assisted serial assertions;
- local principal descriptors, credential verifier, sessions, failed-auth
  throttling, management command schema, authorization matrix, appliance
  status command, controlled-configuration command, and audit correlation;
- bounded network transport framing and deny-by-default network admission;
- local-control and outage-drill QEMU proofs.

The local-control path is a bounded proof surface rather than an interactive
production appliance management console. ITEM-305 connects identity,
configuration, health, network state, and service state to a usable local
view without creating a shell or general command interpreter. In the normal
boot image the view explicitly reports configuration, network, and services
as unavailable until their normal-boot owners are published by later items.

## Network, VirtIO And TLS/HTTPS

Existing reusable primitives:

- virtio PCI identity/capability/status/feature negotiation, virtqueue
  layout/ownership/publication/consumption, DMA region, virtio-net queue
  composition and lifecycle, and virtio-rng queue/request/completion paths;
- packet pool, packet ownership, Ethernet validation, MAC identity, static
  IPv4 profile, netif input/output contracts, pressure metrics, and real
  virtio-net QEMU proofs;
- deterministic lwIP profile, allocator, time, pbuf/packet bridge, netif
  lifecycle, ARP/ICMP, raw TCP listener, four-slot connection pool,
  deadlines, backpressure, stale-callback quarantine, and TCP QEMU stress;
- mbedTLS allocator/DRBG/BIO/handshake/error boundaries, TLS 1.3 QEMU proof,
  HTTP parser/encoder, HTTPS admission, connection composition, lifecycle,
  root/health handlers, client harness, matrix proofs, restart soak, and the
  HTTPS vertical-slice acceptance gate.

There is no production network lifecycle in the normal boot path. The current
IPv4 configuration is a fixed static QEMU profile; the normal image does not
initialize virtio-net, lwIP, entropy, mbedTLS, or HTTPS unless a proof macro
is selected. DHCP and DNS remain explicitly outside the frozen lwIP profile.
The Appliance Platform roadmap intentionally opens those later items; each
must first change the approved lwIP profile boundary and prove the transition
in a focused item.

## Test, Acceptance And CI

Existing reusable primitives:

- Makefile-driven host targets, host-suite runner, QEMU-suite runner, and
  acceptance-suite runner with bounded parallelism;
- deterministic shell fixtures for host/QEMU runners, manifests, and matrix
  verifiers;
- production ELF/ISO marker checks and secret scans;
- release build-input, reproducible-image, artifact-manifest, security,
  operational, disaster-recovery, and Appliance v1 acceptance gates;
- HTTPS vertical-slice acceptance gate;
- GitHub Actions CI that runs ShellCheck, static checks, full `make test`,
  git status checks, and leaked-process checks.

The framework is mature enough to carry the Appliance Platform queue. New
platform items should add focused host fixtures, one deterministic QEMU proof
when behavior is observable, and canonical matrix entries only when a real
aggregate gate is justified.

## Existing Reusable Primitives

| Area | Reusable primitive | Current production proof |
| --- | --- | --- |
| Boot | Limine, GDT/IDT/TSS, PMM/VMM, heap | normal `make iso` boot |
| Interrupts | PIC/IOAPIC/LAPIC, vector `0x42` | normal and dedicated QEMU proofs |
| Process | scheduler, process lifecycle, Ring 3, syscalls | macro-gated QEMU proofs |
| Audit | event ring, structured audit, snapshots | macro-gated QEMU proofs |
| Storage | block device, journal, KV, compaction, backup/restore | macro-gated QEMU proofs |
| Config | schema parser and transactional snapshots | host tests and journal QEMU |
| Services | descriptors, graph, supervisor, health | macro-gated QEMU proof |
| Local control | principals, sessions, auth, authorization, commands | macro-gated QEMU proof |
| Network | virtio, packet pool, static profile, lwIP, TCP | macro-gated QEMU proofs |
| TLS/HTTPS | mbedTLS, HTTP, HTTPS lifecycle | macro-gated QEMU proofs |
| Trust/update | trust, image, update, recovery descriptors | macro-gated QEMU proofs |
| Observability | health, metrics, support bundle, diagnostics | macro-gated QEMU proofs |
| Release | matrices and aggregate gates | `make test-release-appliance-v1-acceptance` |

## Missing Appliance Capabilities

The audit identifies these gaps:

- no stable runtime machine/hostname identity owned by the appliance;
- no production persistent state loader;
- no production network lifecycle;
- no DHCP or DNS resolver in the approved lwIP profile;
- no normal-boot service manager or HTTPS service composition;
- no production HTTPS management service or API;
- no production management authentication, credential initialization, or
  credential recovery;
- no production certificate/secret at-rest activation or approved wrapping
  provider;
- no factory-reset operation with a deterministic state map;
- no integrated platform boot/persistence/management release acceptance;
- no claim that any current QEMU proof is a production-ready appliance.

ITEM-319 publishes the authoritative management authentication architecture
in
[`docs/hfoc-appliance-management-authentication-architecture.md`](docs/hfoc-appliance-management-authentication-architecture.md).
The architecture defines the future `admin` principal, credential lifecycle,
opaque bearer session policy, request enforcement, failure handling, and
Wave E secure defaults. It does not create a production management
authentication path. ITEM-320 implements the bounded administrative
authentication module in
[`docs/hfoc-appliance-administrative-authentication.md`](docs/hfoc-appliance-administrative-authentication.md),
with local credential initialization/recovery, opaque bearer session
publication, and exact request enforcement while preserving the current
`auth=unavailable` management API. ITEM-321 adds the bounded management
authorization and route-policy module in
[`docs/hfoc-appliance-management-authorization.md`](docs/hfoc-appliance-management-authorization.md),
which consumes an ITEM-320 authentication decision, classifies exact
management read routes, and default-denies unknown or unsupported
administrative actions without changing the current API. ITEM-322 owns secret
and credential protection.

ITEM-322 adds the bounded secret and credential protection boundary in
[`docs/hfoc-appliance-secret-protection.md`](docs/hfoc-appliance-secret-protection.md)
and `appliance_secret_protection` under `kernel/`. The module protects an
administrative credential through a canonical one-way SHA-256 fingerprint,
keeps bearer-token/private-key/other-secret material in a bounded volatile
slot with zeroization on rotation/reset, and permits at-rest export only for
the credential fingerprint. Persistent namespaces and certificate/private-key
storage remain owned by ITEM-323 and ITEM-325.

ITEM-323 adds the bounded persistent-storage-namespace boundary in
[`docs/hfoc-appliance-persistent-storage-namespaces.md`](docs/hfoc-appliance-persistent-storage-namespaces.md)
and `appliance_persistent_storage_namespace` under `kernel/`. The module
publishes an immutable inventory of the six roadmap namespace domains
(system configuration, service data, certificates, secrets, diagnostics, and
recovery metadata), their canonical `v1:` record prefixes, and PRIVATE/SECRET
sensitivity labels. A read-only classifier maps bounded record keys to exact
namespace IDs and rejects unknown prefixes. The module performs no allocation,
storage I/O, diagnostic output, or normal-boot change.

ITEM-324 adds the configuration-integrity policy boundary in
[`docs/hfoc-appliance-configuration-integrity.md`](docs/hfoc-appliance-configuration-integrity.md)
and `appliance_config_integrity` under `kernel/`. The module publishes an
immutable catalogue of exact current configuration record families and their
version compatibility, classifies bounded record keys, and plans commit,
last-known-good/default recovery, or explicit journal-failure handling. The
module performs no allocation, storage I/O, or normal-boot change.

ITEM-325 adds the certificate/secret persistence policy boundary in
[`docs/hfoc-appliance-certificate-secret-persistence.md`](docs/hfoc-appliance-certificate-secret-persistence.md)
and `appliance_secret_persistence` under `kernel/`. The module publishes exact
record families under the ITEM-323 `certificate` and `secret` namespaces,
denies raw credential/private-key at-rest record values, and supplies the
HTTPS certificate-dependency readiness contract. The module performs no
allocation, storage I/O, wrapping, or normal-boot change.

ITEM-326 adds the unified appliance health-model boundary in
[`docs/hfoc-appliance-health-model.md`](docs/hfoc-appliance-health-model.md)
and `appliance_health` under `kernel/`. The module is an allocation-free,
read-only evaluator over six fixed domains (boot, storage, configuration,
network, services, and HTTPS management). It publishes exact aggregate
OK/DEGRADED/FAILED/UNKNOWN state, healthy/critical flags, and deterministic
first-fault ordering, and rejects malformed inputs before publishing any
output. The module performs no storage I/O, diagnostics, API, or normal-boot
change.

ITEM-327 adds the bounded runtime diagnostics/log boundary in
[`docs/hfoc-appliance-runtime-diagnostics-logs.md`](docs/hfoc-appliance-runtime-diagnostics-logs.md)
and `appliance_runtime_log` under `kernel/`. The module is an allocation-free
caller-owned ring that publishes exact severity/category/operation/result/
fault/context fields and numeric-only detail, supports explicit full/empty
semantics and non-destructive snapshots, and rejects raw secret/string
payloads by API shape. The module performs no allocation, storage I/O,
diagnostic output, serial/console rendering, audit writes, API route, or
normal-boot change.

## Corrected Execution Order

The original ITEM-302 through ITEM-337 baseline is retained, with two explicit
corrections:

1. Mutating network and service management API endpoints must not precede the
   management authentication, authorization, and secret-protection wave.
2. DHCP/DNS items must first reopen the frozen lwIP profile with a focused
   design, host tests, and a QEMU proof before any production network
   lifecycle can use them.

The corrected execution waves are:

| Wave | Items | Produced boundary |
| --- | --- | --- |
| A | 302-305 | runtime identity, persistent configuration, safe defaults, local console |
| B | 306, 309, 310, 307, 308 | static network persistence and diagnostics before DHCP/DNS profile work |
| C | 311-314 | service manager and HTTP/HTTPS service lifecycle |
| D | 315-318 | management API foundation and read-only system/network/service status |
| E | 319-322 | authentication architecture, admin auth, authorization, secrets |
| F | 317-318 | mutating network/service management APIs only after wave E |
| G | 323-328 | namespaces, persistent integrity, health, diagnostics, operational status |
| H | 329-333 | update architecture, recovery mode, factory reset |
| I | 334-337 | integrated boot, persistence, management, release acceptance |

The dependency rule is: no authenticated administrative capability or
production secret path is exposed before its credential lifecycle,
authorization matrix, and failure/recovery model are implemented and proven.

## Risk Register

- **Proof-surface risk:** Many capabilities are strong modules but are not
  part of normal boot. Platform items must integrate in small PRs with an
  explicit production/publication gate, not just a macro-gated proof.
- **Persistence risk:** Journal, backup, and restore modules exist, but a
  single production state loader must choose namespace, validate schema,
  apply safe defaults, and recover on corrupt state without a general
  filesystem.
- **Network risk:** Enabling DHCP/DNS changes the frozen lwIP profile and
  network ownership boundaries. Each change needs a focused spec/plan and
  strict QEMU evidence.
- **Management risk:** HTTPS service, HTTP parser, and local authorization
  are implemented separately. Wiring them into one authenticated management
  API must be deny-by-default and audited. ITEM-319 now fixes the
  authentication architecture; ITEM-320 through ITEM-322 implement and prove
  the Wave E boundary.
- **Secret risk:** Test certificates are deliberately separated from
  production artifacts. Platform secrets and certificate persistence need
  dedicated namespace, redaction, zeroization, and secret-gate contracts.
- **Scope risk:** The platform must not become a general OS: no arbitrary
  writable filesystem, arbitrary shell, sockets beyond approved transport,
  systemd compatibility, cloud dependency, or accounting business logic.

## Completion Marker

ITEM-301 is complete when the audit, corrected roadmap order, status list,
metadata links, targeted host test, static checks, branch CI, self-review,
and squash merge are all complete. ITEM-302 through ITEM-306, ITEM-309,
ITEM-310, ITEM-307, ITEM-308, ITEM-311, ITEM-312, ITEM-313, ITEM-314, and
ITEM-315, ITEM-316, ITEM-317, and ITEM-318 are also complete. ITEM-317 completed the
read-only network configuration/status boundary; controlled network
configuration operations remain deferred until Wave E. ITEM-318 completed
the read-only service status boundary; controlled service lifecycle
operations remain deferred until Wave E. ITEM-319 completed the management
authentication architecture. ITEM-320 completed the administrative
authentication boundary with no runtime or HTTPS route change. ITEM-321
completed the management authorization and route-policy boundary with no
protected route change. ITEM-322 completed the secret and credential
protection boundary with no protected route, storage, or HTTPS change.
ITEM-323 completed the persistent-storage-namespace boundary with no storage
I/O or boot change. ITEM-324 completed the configuration-integrity policy
boundary with no storage I/O or boot change. ITEM-325 completed the
certificate/secret persistence policy boundary with no storage I/O, wrapping,
or boot change. ITEM-326 completed the unified appliance health-model boundary
with no storage I/O, diagnostics, API, or boot change. ITEM-327 completed the
bounded runtime diagnostics/log boundary with no allocation, storage I/O,
serial/console rendering, API route, or boot change. ITEM-328 adds the HTTPS
health/operational status route in `docs/hfoc-appliance-https-health-operational-status.md`.
ITEM-329 completes the appliance update package architecture in
`docs/hfoc-appliance-update-package-architecture.md`, with no production
transport, storage write, boot-slot mutation, management route, or normal-boot
activation. ITEM-330 completes update manifest and compatibility validation
in `docs/hfoc-appliance-update-manifest-validation.md`, with a bounded
allocation-free parser and no cryptographic verification, storage I/O,
management route, or normal-boot activation. ITEM-331 completes update
integrity and signature verification in
`docs/hfoc-appliance-update-integrity-verification.md`, with SHA-256
metadata/payload checks and update-purpose signed-metadata verification,
without storage I/O, management route, or normal-boot activation. ITEM-332
completes appliance recovery mode in
`docs/hfoc-appliance-recovery-mode.md`, with bounded allocation-free recovery
decisions for configuration, update, and runtime evidence and a
recovery-trust read-only override, without storage I/O, management route, or
normal-boot activation. ITEM-333 completes appliance factory reset in
`docs/hfoc-appliance-factory-reset.md`, with bounded allocation-free evidence
gating and an exact six-namespace state map that clears mutable appliance
state while preserving boot and recovery foundation, without storage I/O,
management route, or normal-boot activation. ITEM-334 completes integrated
appliance boot acceptance in
`docs/hfoc-appliance-integrated-boot-acceptance.md`, with a deterministic
production ISO proof for runtime identity, safe-default configuration, local
management readiness, and clean shutdown, without a persistent-state loader
or storage I/O. ITEM-335 completes persistent configuration reboot
acceptance in
`docs/hfoc-appliance-persistent-config-reboot-acceptance.md`, with a
deterministic three-boot macro-gated QEMU proof for committed hostname
reload and schema-invalid recovery, without a production normal-boot state
loader or real disk driver. ITEM-336 completes management plane QEMU
acceptance in
`docs/hfoc-appliance-management-plane-qemu-acceptance.md`, with the
macro-gated administrative authentication and authorization fixtures run
before the existing real TLS 1.3 HTTPS management proof. ITEM-337 completes
HFOC Appliance Platform release acceptance in
`docs/hfoc-appliance-platform-release-acceptance.md`, with an exact final
matrix of boot, persistence, management, network, services, HTTPS, security,
recovery, update, reset, and release proof owners. The HFOC Appliance
Platform roadmap is complete.
