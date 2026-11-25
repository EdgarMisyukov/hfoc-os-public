# HFOC OS v1.0.0 release checks / Проверки выпуска HFOC OS v1.0.0

The following excerpts support release verification.
Ниже приведены выдержки для проверки выпуска.

```text
### IOAPIC Stage 2 IRQ0 shadow routing
  remains the authoritative timer owner and continues to call
  `ioapic_route_irq(1, 0x41)` → `ioapic_route_irq(0, 0x40)` →
ISA IRQ3–7 use verified IOAPIC vectors 0x44–0x48; PIC IRQ3–7 lines are masked only after route publication and remain fallback otherwise.
ISA IRQ8–15 use verified IOAPIC vectors 0x49–0x50; PIC IRQ8–15 lines are masked only after route publication and remain fallback otherwise.
PIC IRQ2 is permanently reserved as the 8259 cascade line; direct pic_unmask(2U) is rejected and no IRQ2 endpoint exists.
### IOAPIC Stage 3a PIC IRQ1 masking
использует `irq1_ready` conjunction
  `[PIC ] IRQ1 masked; IOAPIC vector=0x41 active.`;
  `[PIC ] Master mask: 0x00000000000000FE; slave mask: 0x00000000000000FF`;
- QEMU `test-qemu-ioapic-irq1.sh` сохраняет `trace_offset` перед sendkey
  `INT=0x41` и `INT=0x21 absent`; absence `0x21` не проверяется
Stage 3b PIC IRQ0 masking реализован в `main`.
- dispatcher использует только `irq0_ioapic_active`; no dispatcher IOAPIC reread: `interrupt_dispatch()` не включает `ioapic.h` и не вызывает `ioapic_is_irq0_routed()`;
  как stray PIC EOI only: ровно `pic_send_eoi(0)`, без timer call и без LAPIC
- fallback preserves PIC vector 0x20 timer ownership: `timer_handle_irq()` +
  `[PIC ] Master mask: 0x00000000000000FF; slave mask: 0x00000000000000FF`;
  `[PIC ] Master mask: 0x00000000000000FC; slave mask: 0x00000000000000FF`;
- verified IOAPIC route makes vector 0x40 authoritative PIT timer owner
  verified successful APIC runtime masks both PIC controllers to FF/FF;
  policy; logical mask disable, not IMCR/hardware removal; IRQ8–15 routing is
- completion remasks one-shot LVT и точно восстанавливает saved periodic 0x42
  and leaves `IF=0`;
PS/2 Set 1 decoder and console line editing: US Set 1, Shift/CapsLock, E0
ANSI. IRQ1 vector `0x41` ownership unchanged. The fixed 16-entry command history
uses non-empty newest-only deduplication and exact draft restore. No persistence, completion/search, ANSI, UTF-8, keypad, or heap.
Portable QEMU xAPIC baseline: q35, one vCPU, QEMU default xAPIC-capable CPU; -cpu max,-apic is not required.
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
strict `[CONFIG-JOURNAL]` markers.
## Bounded configuration/audit checkpoint KV API
## State schema registry
## State migration transaction
## Journal compaction commit
## Deterministic backup stream
## Backup manifest inventory
## Bounded backup producer
## Restore validation and no-change rollback
## Restore dry-run planner
## Staged restore activation
## Backup/restore QEMU acceptance
## Service dependency descriptor validation
## Dependency graph snapshots
## BSP service supervisor lifecycle
readiness/liveness state: `service_supervisor_report_ready()` publishes
## Ordered service start
## Ordered service stop
## Restart policy descriptor
## Service restart containment
## Service resource envelope
## Service readiness deadline
## Service state snapshot
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
The header defines `HFOC_MBEDTLS_CONFIG_VERSION 5` (ITEM-508 increment 33,
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
item 273 completed header section. The host-only framing gate
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
engine to answer and rate-limit echo requests. `make test-arp-icmp-qemu`
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
support-bundle exporter boundary. `support_bundle_exporter_init()` validates
## Diagnostic privacy policy
The fixed `HFOC-DIAG-PRIV-1` policy classifies exact secret-bearing
sensitive bytes. The support-bundle manifest and exporter use this shared
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
## Trust-policy QEMU proof
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
`docs/hfoc-appliance-management-authentication-architecture.md` is the
`docs/hfoc-appliance-administrative-authentication.md` and
`docs/hfoc-appliance-management-authorization.md` and
`docs/hfoc-appliance-secret-protection.md` and
`docs/hfoc-appliance-persistent-storage-namespaces.md` and
`docs/hfoc-appliance-configuration-integrity.md` and
`docs/hfoc-appliance-certificate-secret-persistence.md` and
`docs/hfoc-appliance-health-model.md` and
`docs/hfoc-appliance-runtime-diagnostics-logs.md` and
`docs/hfoc-appliance-https-health-operational-status.md` and
`docs/hfoc-appliance-update-package-architecture.md` is the ITEM-329
`docs/hfoc-appliance-update-manifest-validation.md` and
## Appliance update integrity verification
`docs/hfoc-appliance-update-integrity-verification.md` and
## Appliance recovery mode
`docs/hfoc-appliance-recovery-mode.md` and
## Appliance factory reset
`docs/hfoc-appliance-factory-reset.md` and
## Integrated appliance boot acceptance
`docs/hfoc-appliance-integrated-boot-acceptance.md` is the ITEM-334
## Persistent configuration reboot acceptance
`docs/hfoc-appliance-persistent-config-reboot-acceptance.md` is the ITEM-335
## Management plane QEMU acceptance
`docs/hfoc-appliance-management-plane-qemu-acceptance.md` is the ITEM-336
## HFOC Appliance Platform release acceptance
`docs/hfoc-appliance-platform-release-acceptance.md` and
final HFOC Appliance Platform release gate. The canonical matrix records
physical NVMe hardware acceptance remains `NOT_TESTED`.
## UEFI installer helper
## Production auth/authz request integration
## Installer architecture audit
## Hardware and existing OS discovery
## Clean installer layout
## Bootloader recovery evidence
## First boot wizard
## Appliance boot console
## Management web UI
reachability policy. ITEM-382 adds the `about/licenses` page to that panel: the
## Integrated installer, dual-boot and management acceptance
## Release documentation
## Production NIC/lwIP backend
ITEM-505 removes the read-only property from the network target alone.  The
ITEM-505 increment 2 gives the generic sections their live view on a real
ITEM-505 increment 3 makes the System section the third configuring section and
ITEM-506 increment 1 puts the credential model itself in place before the
ITEM-506 increment 2 gives that model its owner and its conversation, still
ITEM-506 increment 3 switches the two interfaces to the account and removes the
ITEM-506 increment 4 sets the account at install time.  The conversation is the
`admin=SET` over a store that does not hold it.  `apply()` publishes
`[PRODUCTION-SERVING] v=1 state=AWAITING_CREDENTIAL
ITEM-506 increment 6 makes the one account changeable from the management
ITEM-391 adds `kernel/src/installer_layout_contract.c` and
footprint 329728 + GPT tail 34 = 331810 sectors) is refused as
ITEM-397 делает установленный диск загружаемым: установщик пишет в ESP
## Production service and HTTPS activation
ITEM-369 adds the production end-to-end acceptance gate for the production
ITEM-371 adds signed media and installer hardening in
ITEM-372 adds the signed boot chain and boot attestation in
ITEM-373 adds storage encryption and key custody in
ITEM-374 adds network modes, time synchronization and offline bring-up in
break-glass recovery in `docs/hfoc-support-bundle-redaction-break-glass.md`.
ITEM-383 adds the trademark and certified-build policy in
CVE process, reproducible image digest) owns the artefacts a certified build
  `HOST_PROVISIONED` block is mixed through the same chain. The hidden
`HFOC_LWIP_PROTO_ICMP_WINDOW_MS` (1000 ms) window and drops the rest before
## USB HID: все контроллеры, все порты, каждое устройство называет себя (ITEM-452)
— `bar=0xFCEA0000 ports=14 slots=127 vendor=0x1022 device=0x43EC
## Фасад: таблица маршрутов имя → backend (ITEM-504, первый инкремент)
Первый инкремент пункта — `kernel/src/appliance_route_table.c` и
Второй инкремент пункта — `kernel/src/appliance_route_config.c` и
`appliance_production_serving_handle_routing` в
Четвёртый инкремент открывает страницу Routing — первый раздел панели,
требует `[PRODUCTION-ROUTES] source=COMMITTED routes=3; …`),
поднимает релей (`appliance_production_serving_relay_start`), который
appliance_certificate_name_store.c` и `kernel/include/
  объявлен, отклоняется с `ROUTE_WITHOUT_CERTIFICATE` **даже при объявленном
следующей загрузке — `name=unknown.example result=REFUSED cert=none
## Два имени — два сертификата: QEMU-фикстура выбора (ITEM-507, инкремент 6)
## ITEM-508 increment 11 — драйвер обмена ACME: окна, шов и одна запись (04.10)
## ITEM-508 increment 12 — исключение символа mbedTLS: один символ, один файл (04.10)
## ITEM-508 increment 13 — привязка lwIP и mbedTLS: байты двигает, решений не принимает (04.10)
## ITEM-508 increment 15 — обмен директории: проба говорит с заглушкой настоящей привязкой (05.10)
## ITEM-508 increment 17 — заказ ACME целиком: поток, транспорт и привязка в одной пробе (05.10)
## ITEM-508 increment 18 — опрос челленджа: клиент ждёт `processing` (05.10)
## ITEM-508 increment 19 — опрос заказа: finalize может быть ещё pending (05.10)
## ITEM-508 increment 20 — продление: цепочка, срок и решение (05.10)
## ITEM-508 increment 22 — finalize несёт настоящий PKCS#10 (05.10)
## ITEM-508 increment 23 — выданная цепочка хранится со своим происхождением (05.10)
## ITEM-508 increment 24 — ключ аккаунта ACME — это сохранённый ключ личности (05.10)
## ITEM-508 increment 25 — драйвер заказа связывает аккаунт через модуль (05.10)
## ITEM-508 increment 26 — служба ACME на обычной загрузке (05.10)
## ITEM-508 increment 27 — пул слушателей TCP (05.10)
## ITEM-508 increment 28 — слушатель HTTP-01 (06.10)
## ITEM-508 increment 29 — провод HTTP-01 и фикстурное вооружение (06.10)
## ITEM-508 increment 30 — путь заказа вооружает слушатель (06.10)
## ITEM-508 increment 31 — заказ вооружает слушатель, и ответ уходит на провод (06.10)
## ITEM-508 increment 32 — панель показывает состояние ACME (только чтение) (06.10)
## ITEM-508 increment 34 — управление ACME: включение, выключение и заявка (06.10)
## ITEM-508 increment 35 — путь заказа на обычной загрузке (06.10)
## ITEM-508 increment 36 — проводное доказательство production-заказа (06.10)
## ITEM-508 increment 37 — панель показывает ход заказа (06.10)
## ITEM-509 increment 3 — кириллица в консоли и UTF-8 в пути вывода (07.10)
## ITEM-509 increment 4 — команда `language`: первая поверхность, читающая настройку (07.10)
## ITEM-509 increment 5 — панель показывает язык (первая половина 509.4) (07.10)
## ITEM-509 increment 6 — переключатель языка в панели (вторая половина 509.4) (07.10)
## ITEM-509 increment 7 — контент помощи и консольный рендерер (509.5) (07.10)
## ITEM-509 increment 8 — рендерер помощи в панели (509.6) (07.10)
## ITEM-509 increment 9 — меню говорит на выбранном языке (509.7b) (07.10)
## ITEM-509 increment 10 — каркас панели и `<html lang>` (509.7a-1) (07.10)
## ITEM-509 increment 27 — язык объявляет носитель (509.7c-2) (09.10)
## ITEM-512 — команды консоли не зависят от регистра (10.10)
## ITEM-513 — язык панели как свойство браузера
## ITEM-514 — экран схем установки
## ITEM-515 — форма сети и общий разбор форм панели
## ITEM-516: настраиваемый гейт управления (10.10)
## ITEM-517 — установка без UEFI / legacy BIOS
## ITEM-518 — диагностика установщика: показать наблюдённое (11.10)
## USB 2.0: EHCI и загрузочный носитель (ITEM-551)
не пишется.  Записи: `[EHCI]`, `[EHCI-PORT]`, `[EHCI-STORAGE]`,
## Экспорт диагностики файлом (ITEM-552)
загрузки и проверяет предвыделенный `\HFOC\INSTALL.LOG` тем же
ITEM-590 moved the `[NET-PHY]` record's composition and its boundary into
ITEM-591 gave the PHYAR handshake its measured time: `rtl8168_phy_wait()` polls
ITEM-592 made the RTL8168 cold start profile-driven.  The XID table's row now
### ITEM-595: PHYAR completion and payload are separate reads
### ITEM-596: preserve both sides of refused PHY page selection
```
