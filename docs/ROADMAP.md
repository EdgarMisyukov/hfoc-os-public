# HFOC OS v1.0.0 release checks / Проверки выпуска HFOC OS v1.0.0

The following excerpts support release verification.
Ниже приведены выдержки для проверки выпуска.

```text
- ITEM-645 (implemented): the poll-driven RTL8168 receive path acknowledges
- ITEM-643 (implemented): the receive path publishes a read-only `[NET-RX]`
- ITEM-642 (implemented): the product release gate owns both clean builds; it
- ITEM-641 (implemented): proof preflights and every marker assertion in
  and real TLS 1.3 QEMU proof coverage.
  read-only route, a validated caller-owned identity/health/config snapshot,
- ITEM-319 (реализован): management authentication architecture in
- ITEM-320 (реализован): administrative authentication in
- ITEM-321 (реализован): management authorization in
- ITEM-322 (реализован): secret and credential protection in
- ITEM-323 (реализован): persistent storage namespaces in
- ITEM-324 (реализован): configuration integrity in
- ITEM-325 (реализован): certificate and secret persistence policy in
- ITEM-326 (реализован): appliance health model in
- ITEM-327 (реализован): runtime diagnostics and logs in
- ITEM-328 (реализован): HTTPS health and operational status in
- ITEM-329 (реализован): appliance update package architecture in
- ITEM-330 (реализован): update manifest and compatibility validation in
- ITEM-331 (реализован): update integrity and signature verification in
- ITEM-332 (реализован): appliance recovery mode in
- ITEM-333 (реализован): appliance factory reset in
- ITEM-334 (реализован): integrated appliance boot acceptance in
- ITEM-335 (реализован): persistent configuration reboot acceptance in
- ITEM-336 (реализован): management plane QEMU acceptance in
- ITEM-337 (реализован): final platform release acceptance gate in
- ITEM-338 (реализован): installer architecture and storage-safety audit in
  `docs/hfoc-hardware-os-discovery.md` and
- ITEM-342 (реализован): bootloader recovery evidence and plans in
- ITEM-345 (реализован): bounded HTTPS management web UI shell in
- ITEM-346 (реализован): integrated installer/dual-boot/management
- ITEM-355 (реализован): first-boot integration boundary in
  pending and must be recorded in the Hardware Acceptance Report.
- ITEM-361 (реализован): bounded pre-kernel UEFI installer helper in
- ITEM-362A (реализован): ordinary-boot production NIC/lwIP backend in
- ITEM-391 (реализован): production install layout contract in
  `docs/hfoc-installer-layout.md`,
  (331810 sectors, >= 162 MiB) is refused as
- ITEM-364 (реализован): ordinary-boot production service/HTTPS activation in
- ITEM-365 (реализован): production auth/authz request integration in
  HFOC Appliance Platform release gate.
  authorization matrix. Host proof `make
  Physical factory reset remains `NOT_TESTED`.
- ITEM-369 (реализован): production end-to-end acceptance in
  `/panel/about/licenses` (не десятый раздел: модель девяти разделов и
ITEM-397 закрывает петлю «установка → перезагрузка → управление»: установщик
- ITEM-383 (реализован): trademark and certified-build policy in
  сертификационная mark на build без действительного certified build label.
- ITEM-371 (реализован): signed media and installer hardening in
- ITEM-372 (реализован): signed boot chain and boot attestation in
- ITEM-373 (реализован): storage encryption and key custody in
  bring-up in `docs/hfoc-production-network-time-offline.md`.
- ITEM-385 (реализован): management reachability policy in
- ITEM-378 (реализован): support bundle, redaction и break-glass recovery in
- IOAPIC IRQ0 shadow route (Stage 2, реализован в текущей ветке; PIC vector
- Stage 3a PIC IRQ1 masking (реализован в `main`; PIC IRQ1 маскируется после verified IOAPIC IRQ1 route);
- Stage 3b PIC IRQ0 masking (реализован в `main`; при success PIC IRQ0 маскируется, authoritative PIT ticks идут через vector `0x40`, fallback сохраняет PIC vector `0x20`);
- transactional legacy PIC disable (реализован в текущей ветке; verified
- test-only LAPIC timer one-shot probe (реализован в текущей ветке; normal
  transactional vector `0x43` и точное восстановление periodic 0x42;
- portable QEMU xAPIC baseline: APIC-проверки на q35 / один vCPU / default CPU
- PS/2 Set 1 decoder, line editing консоли и fixed command history (реализован
  fixed 16-entry ring, non-empty newest-only dedup, exact draft restore,
- immutable security-domain identity and syscall-origin validation (реализован
- capability-gated syscall operations (реализован в текущей ветке; exact
  `make test-event-ring-host`).
- structured audit records (реализован в текущей ветке; fixed process,
- consistent audit/event snapshots (реализован в текущей ветке;
- strict appliance configuration schema (реализован в текущей ветке;
- static system-image manifest (реализован в текущей ветке; bounded versioned
- pre-publication image integrity (реализован в текущей ветке; bounded gate
  verified recovery image from the boot policy; both failed candidates leave
  `make test-boot-policy-regression`).
  QEMU proof `make test-block-device`).
  `make test-data-region`).
  `make test-block-cache`).
  `make test-config-journal`).
  `make test-config-kv-host`).
  `make test-state-schema-host`).
  `make test-state-migration-host`).
  `make test-journal-compaction-host`).
  without mutation; host-only proof `make test-backup-stream-host`).
- backup stream framing v1 (реализован в текущей ветке; explicit framing
- backup manifest inventory (реализован в текущей ветке; fixed 160-byte
- bounded backup producer (реализован в текущей ветке; back-pressure-safe
- restore dry-run planner (реализован в текущей ветке; complete backup
- staged restore activation (реализован в текущей ветке; validated restore
- backup/restore QEMU acceptance (реализован в текущей ветке; one QEMU session
- service dependency descriptors (реализован в текущей ветке; bounded
- dependency graph snapshot (реализован в текущей ветке; immutable
- ordered service start (реализован в текущей ветке; consumes the immutable
- ordered service stop (реализован в текущей ветке; stops running snapshot
  host-only proof `make test-service-restart-policy-host`).
- service restart containment (реализован в текущей ветке; per-service
- service resource envelope (реализован в текущей ветке; descriptor CPU and
  user-memory limits are attached to every owned service process, direct
- service readiness deadline (реализован в текущей ветке; bounded monotonic
  arm fresh deadlines; host-only proof
- service state snapshot (реализован в текущей ветке; bounded read-only
- supervisor composition QEMU proof (реализован в текущей ветке; one q35 /
- local principal descriptor (реализован в текущей ветке; bounded immutable
  `make test-credential-verifier-host`).
- local session lifecycle (реализован в текущей ветке; bounded caller-owned
- failed-authentication throttle (реализован в текущей ветке; bounded
  `make test-management-command-host`).
- read-only appliance status command (реализован в текущей ветке; bounded
  `make test-controlled-configuration-host`).
  proof `make test-audit-correlation-host`).
- local-control QEMU proof (реализован в текущей ветке; one q35 / one-vCPU /
- network device descriptor (реализован в текущей ветке; single selected
- link-state model (реализован в текущей ветке; explicit
  proof `make test-virtio-pci-identity-host`).
  proof `make test-virtio-pci-capability-host`).
- virtio device status lifecycle (реализован в текущей ветке; item 203 of the
  `make test-virtio-pci-feature-host`).
  allocation; host-only proof `make test-virtio-queue-layout-host`).
- virtqueue descriptor ownership (реализован в текущей ветке; item 206 of
- virtqueue available publication (реализован в текущей ветке; item 207 of
- virtqueue used consumption (реализован в текущей ветке; item 208 of the
  `make test-virtio-dma-region-host`).
  transition; QEMU proof `make test-virtio-foundation-qemu`).
  `make test-virtio-net-feature-profile-host`).
  proof `make test-virtio-net-queue-composition-host`).
  `make test-virtio-net-rx-host`).
- virtio-net TX reclaim (реализован в текущей ветке; item 216 of the HTTPS
  `make test-virtio-net-interrupt-host`).
  proof `make test-virtio-net-link-state-host`).
- virtio-net reset and shutdown (реализован в текущей ветке; item 219 of the
  proof `make test-virtio-net-frame-qemu`).
- fixed IPv4 configuration parser (реализован в текущей ветке; strict
- bounded packet buffer ownership (реализован в текущей ветке; fixed
- network packet-pool descriptor (реализован в текущей ветке; fixed packet
  count balance; host-only proof `make test-packet-lifecycle-host`).
  and no pointer escape; host-only proof `make test-packet-slice-host`).
  `make test-ethernet-frame-host`).
  `make test-virtio-net-mac-identity-host`).
- static QEMU IPv4 profile binding (реализован в текущей ветке; item 226 of
  profile through the existing network configuration transaction, binds item
- HFOC netif input contract (реализован в текущей ветке; item 227 of the
- HFOC netif output contract (реализован в текущей ветке; item 228 of the
  `make test-packet-pressure-host`).
  `make test-packet-boundary-qemu`).
- mbedTLS source and license lock (реализован в текущей ветке; item 261 of
- minimal mbedTLS TLS 1.3 configuration (реализован в текущей ветке;
- mbedTLS allocator boundary (реализован в текущей ветке; item 263 of the
- mbedTLS entropy/DRBG adapter (реализован в текущей ветке; item 264 of the
- mbedTLS certificate/key manifest parser (реализован в текущей ветке;
- mbedTLS test-secret build separation (реализован в текущей ветке;
- mbedTLS BIO transport adapter (реализован в текущей ветке; item 267 of the
- TLS handshake state machine (реализован в текущей ветке; item 268 of the
- TLS close and error normalization (реализован в текущей ветке; item 269 of
- HTTP parser state descriptor (реализован в текущей ветке; item 271 of the
- Incremental request-line parser (реализован в текущей ветке; item 272 of
- Incremental header parser (реализован в текущей ветке; item 273 of the
- HTTP framing and body rejection (реализован в текущей ветке; item 274 of
- HTTP method/version and Host policy (реализован в текущей ветке; item 275
- HTTP fixed route table (реализован в текущей ветке; item 276 of the HTTPS
- HTTP deterministic response encoder (реализован в текущей ветке; item 277
- HTTP HEAD response semantics (реализован в текущей ветке; item 278 of the
- HTTP error matrix (реализован в текущей ветке; item 279 of the HTTPS
- HTTP parser/encoder host acceptance (реализован в текущей ветке; item 280
- HTTPS service descriptor (реализован в текущей ветке; item 281 of the
- HTTPS admission transaction (реализован в текущей ветке; item 282 of the
- HTTPS connection composition (реализован в текущей ветке; item 283 of the
- HTTPS TLS-to-HTTP data flow (реализован в текущей ветке; item 284 of the
- HTTPS root and health handlers (реализован в текущей ветке; item 285 of
- HTTPS health/audit/metrics (реализован в текущей ветке; item 286 of
- HTTPS fifth-connection denial (реализован в текущей ветке; item 287 of the
- HTTPS peer-failure throttle (реализован в текущей ветке; item 288 of the
- HTTPS graceful stop and exact restart (реализован в текущей ветке; item
- HTTPS service QEMU proof (реализован в текущей ветке; item 290 of the
- Standard HTTPS client harness (реализован в текущей ветке; item 291 of the
- Exact HTTPS successful response matrix (реализован в текущей ветке; item
- Encrypted HTTPS malformed-request matrix (реализован в текущей ветке; item
- Encrypted HTTPS size-boundary matrix (реализован в текущей ветке; item 294
- Concurrent HTTPS exhaustion matrix (реализован в текущей ветке; item 295 of
- Entropy fail-closed matrix (реализован в текущей ветке; item 296 of the
- Certificate/key fail-closed matrix (реализован в текущей ветке; item 297 of
- Production artifact secret gate (реализован в текущей ветке; item 298 of
- HTTPS restart/soak rehearsal (реализован в текущей ветке; item 299 of the
  explicit `HTTPS vertical-slice acceptance gate: ACCEPT/REJECT`, no new
- lwIP pbuf/packet ownership bridge (реализован в текущей ветке; item 235 of
  vertical slice, fixed one-netif lifecycle staging MAC/MTU/static
  shutdown; proof `make test-arp-icmp-qemu`).
- raw TCP listener contract (реализован в текущей ветке; item 238 of the
- TCP connection lifecycle adapter (реализован в текущей ветке; item 239 of
- Four-slot connection pool (реализован в текущей ветке; item 241 of the
  `make test-lwip-tcp-backlog-host`).
  `make test-lwip-tcp-rx-backpressure-host`).
  retry neither skips nor duplicates bytes; host-only proof
- Stale callback quarantine (реализован в текущей ветке; item 246 of the
- Malformed TCP isolation matrix (реализован в текущей ветке; item 247 of
- Listener drain and restart (реализован в текущей ветке; item 249 of the
  aborts expired connections through the item 248 close policy, removes the
  proof `make test-tcp-listener-qemu`).
  `make test-network-resource-qemu`).
  proof `make test-virtio-rng-feature-profile-host`).
  `make test-virtio-rng-queue-composition-host`).
- Virtio-rng random request API (реализован в текущей ветке; item 253 of the
- Virtio-rng completion path (реализован в текущей ветке; item 254 of the
- Entropy continuous health (реализован в текущей ветке; item 255 of the
  `make test-entropy-accumulator-host`).
  `make test-entropy-seed-fill-host`).
  `make test-entropy-diagnostics-host`).
  `make test-entropy-restart-host`).
  proof `make test-entropy-qemu`).
- minimal ICMP reachability (реализован в текущей ветке; bounded echo
- management transport framing (реализован в текущей ветке; versioned
- network admission policy (реализован в текущей ветке; deny-by-default
- network audit/health events (реализован в текущей ветке; bounded
  `make test-verification-policy-host`).
  backup/integration/recovery purpose masks on trust anchors,
  `make test-key-use-host`).
  publication; host-only proof `make test-signed-metadata-host`).
  `make test-trust-rotation-host`).
  `make test-trust-revocation-host`).
  `make test-image-policy-host`).
  proof `make test-boot-evidence-host`).
  mode; host-only proof `make test-recovery-trust-host`).
- trust-policy QEMU proof (реализован в текущей ветке; one q35 / one-vCPU /
  `make test-update-compatibility-host`).
  `make test-candidate-slot-inventory-host`).
  proof `make test-candidate-preflight-host`).
- candidate staged write verification (реализован в текущей ветке; writes a
  activation intent or health stores; host-only proof
  `make test-automatic-rollback-host`).
  `make test-update-acceptance-host`).
- update retention policy (реализован в текущей ветке; bounded keep policy
- update lifecycle QEMU proof (реализован в текущей ветке; one q35 /
- health-check descriptor (реализован в текущей ветке; bounded 16-entry
- storage health check (реализован в текущей ветке; validates compacted
- service health aggregation (реализован в текущей ветке; aggregates
- network health aggregation (реализован в текущей ветке; consumes published
- audit-ring pressure health (реализован в текущей ветке; consumes a
- versioned metrics snapshot (реализован в текущей ветке; fixed schema
- support-bundle manifest (реализован в текущей ветке; fixed 512-byte
- support-bundle exporter (реализован в текущей ветке; bounded `HFOCSBEX`
- diagnostic privacy policy (реализован в текущей ветке; fixed
- supportability QEMU proof (реализован в текущей ветке; q35 / one-vCPU /
- external integration principal scope (реализован в текущей ветке; dedicated
- external integration session handshake (реализован в текущей ветке; bounded
- accounting hand-off envelope v1 (реализован в текущей ветке; deterministic
- idempotency ledger (реализован в текущей ветке; bounded 64-entry
- outbound event cursor (реализован в текущей ветке; bounded FIFO audit/event
  external configuration intents with exact field ranges, local
- integration failure isolation (реализован в текущей ветке; bounded 64-peer
- integration compatibility policy (реализован в текущей ветке; explicit
- accounting-boundary QEMU proof (реализован в текущей ветке; q35 / one-vCPU /
- fault-injection catalogue (реализован в текущей ветке; versioned capacity-24
- storage interruption drill (реализован в текущей ветке; composed host and
- service cascade drill (реализован в текущей ветке; focused host and QEMU
- local-control outage drill (реализован в текущей ветке; focused host and
- network partition drill (реализован в текущей ветке; focused host and QEMU
- trust failure drill (реализован в текущей ветке; focused host and QEMU drill
- update failure drill (реализован в текущей ветке; focused host and QEMU
  first-boot health selects known-good deterministically; no production
- integration replay/outage drill (реализован в текущей ветке; focused host
- power-cycle continuity drill (реализован в текущей ветке; focused host and
  `make test-resilience-matrix-qemu`).
- release profile descriptor (реализован в текущей ветке; immutable
  `make test-build-input-manifest-host`).
  `make test-reproducible-image-host`).
  proof `make test-release-artifact-host`).
  `make test-release-upgrade-matrix`).
  backup/restore, allowed image 2→1 restore/rollback, rejected
  `make test-release-backup-restore-matrix`).
  `make test-release-security-baseline`).
  proof `make test-release-operational-baseline`).
- appliance v1 disaster-recovery rehearsal (реализован в текущей ветке;
- appliance v1 acceptance gate (реализован в текущей ветке; canonical
- BSP service supervisor lifecycle (реализован в текущей ветке; fixed
- service readiness and liveness (реализован в текущей ветке; verified READY
The ITEM-386 scratch question is measured and closed by ITEM-388. The scratch
- ITEM-444 (реализован): на экране строчные буквы. Прогон 18.09.2026 на железе
- ITEM-458 (сделан): клавиатура не теряет символы и называет то, что отбросила.
  пропускает символы» во время прокрутки экрана. ITEM-457 убрал остановку
- ITEM-459 (сделан): у каждого печатного usage есть символ, а мёртвая клавиша
- ITEM-461 (сделан): консоль успевает за вводом, и это измерено. Владелец на
- ITEM-462 (сделан): диагностика CI больше не съедает хранилище GitHub.
- ITEM-464 (сделан): установленная система действительно грузится в
* ITEM-467 (СРОЧНО-17, раунд 6, **сделано**): ввод переживает поток в сети, а
* ITEM-468 (СРОЧНО-18, раунд 7, **сделано**, диагностика): машина сама
  ответ идёт в durable-журнал через транскрипт ITEM-460.  Поведение не
* ITEM-469 (СРОЧНО-19, раунд 8, **сделано**): клавиатура держит очередь
  часы называют себя.  Прогон владельца после ITEM-470 принёс отчёты мыши
  гейна и честное время.  Прогон владельца после ITEM-471 показал
  а у глубины есть ручка.  Прогон владельца после ITEM-472 показал
* ITEM-474 (СРОЧНО-24, раунд 13, **сделано**): оборот по фазам и пакетная
  прокрутка.  Прогон владельца после ITEM-473 дал здоровый транспорт мыши
* ITEM-475 (СРОЧНО-25, раунд 14, **сделано**): тик на PIT в production.
* ITEM-476 (раунд 14, follow-up, **сделано**): фазы описывают те обороты, рядом
  приглашение и то, что попросил оператор.  ITEM-477 убрал с экрана лишь
  замершего эндпоинта.  Журнал прогона ITEM-478
* ITEM-480 (СРОЧНО-29, раунд 18, **сделано**): бюджет оборота для мыши и
* ITEM-481 (СРОЧНО-30, раунд 19, **сделано**): граница durable-записи
* ITEM-482 (СРОЧНО-31, раунд 20, **сделано**): набор идёт одним знакоместом,
* ITEM-483 (доработка раунда 20, **сделано**): после чужого вывода быстрый путь
  Первая попытка прогона ITEM-484 (`f2400e52`, run 36230645539) уронила
* ITEM-486 (раунд 23, **сделано**): `other_us` называет свои части.  Журнал
`docs/hfoc-hardware-acceptance.md` (стадия `NETWORK_CHOICE`).  Аппаратного
## ITEM-497 — «ввод не поднялся на первой загрузке — и это нечем разобрать»
  трогает: наблюдаемость ITEM-479/480/486 не ослаблена.
  `docs/hfoc-appliance-boot-console.md`.
## ITEM-498 — «отказ называет свой статус и причину, а не одно слово»
## ITEM-499 — «меню панели ведёт туда, что машина обслуживает»
## ITEM-500 — «медиа, объявленная для одной машины, ставится на любую»
## ITEM-502 — «переустановка воскрешает состояние предыдущего прогона»
## ITEM-503 — «сертификат, которому доверяет браузер»: свой сертификат оператора (BYO)
## ITEM-504 — фасад: таблица маршрутов имя → backend (первый инкремент)
`[PRODUCTION-ROUTES] source=COMMITTED routes=3; name=a.example …`.
## ITEM-505 — всё, что настроено в панели, сохраняется в HFOC OS (инкремент 1: сеть)
## ITEM-505 increment 2 — живое представление панели на реальном аппарате
## ITEM-505 increment 3 — раздел System настраивает имя хоста
  пользуется first-boot wizard, — и публикует
## ITEM-506 increment 1 — модель учётной записи: логин и PBKDF2-verifier
## ITEM-506 increment 2 — владелец учётной записи и разговор консоли
## ITEM-506 increment 3 — консоль и панель входят одной учётной записью
  дважды), `login` открывает локальную сессию, а break-glass reset ITEM-465
* `apply()` публикует `[PRODUCTION-INSTALL] admin=SET login=<login>
## ITEM-507 increment 1 — SNI входит в замороженный профиль (04.10)
## ITEM-507 increment 2 — имя → сертификат и ключ: модель и энергонезависимая запись (04.10)
## ITEM-507 increment 3 — SNI выбирает сертификат по имени (04.10)
## ITEM-507 increment 4 — мутация «имя → сертификат и ключ» и согласованность с маршрутами (04.10)
## ITEM-507 increment 5 — раздел Certificates: панель управляет именами (04.10)
## ITEM-507 increment 6 — два имени получают два сертификата (04.10)
## ITEM-508 increment 11 — драйвер обмена ACME: окна, шов и одна запись (04.10)
## ITEM-508 increment 12 — исключение символа mbedTLS: один символ, один файл (04.10)
## ITEM-508 increment 13 — привязка lwIP и mbedTLS (04.10)
## ITEM-508 increment 15 — обмен директории в QEMU поверх настоящей привязки (05.10)
## ITEM-508 increment 17 — заказ ACME целиком поверх настоящей привязки (05.10)
## ITEM-508 increment 18 — клиент ждёт `processing` (05.10)
## ITEM-508 increment 19 — клиент опрашивает и заказ (05.10)
## ITEM-508 increment 20 — проверенная цепочка питает политику продления (05.10)
## ITEM-508 increment 22 — finalize несёт настоящий PKCS#10 (05.10)
## ITEM-508 increment 23 — выданная цепочка хранится со своим происхождением (05.10)
## ITEM-508 increment 24 — ключ аккаунта ACME — это сохранённый ключ личности (05.10)
## ITEM-508 increment 25 — драйвер заказа связывает аккаунт через модуль (05.10)
## ITEM-508 increment 26 — служба ACME на загрузке: аккаунт и политика продления (05.10)
## ITEM-508 increment 27 — обёртка TCP держит два слушателя (05.10)
## ITEM-508 increment 28 — слушатель HTTP-01 (06.10)
## ITEM-508 increment 29 — ответ HTTP-01 на настоящем проводе (06.10)
## ITEM-508 increment 30 — путь заказа вооружает слушатель (06.10)
## ITEM-508 increment 31 — заказ вооружает слушатель, и ответ уходит на провод (06.10)
## ITEM-508 increment 32 — панель показывает состояние ACME (только чтение) (06.10)
## ITEM-508 increment 33 — проверка RSA для настоящей цепочки Let's Encrypt (06.10)
## ITEM-508 increment 34 — управление ACME: включение, выключение и заявка (06.10)
## ITEM-508 increment 35 — путь заказа на обычной загрузке (06.10)
## ITEM-508 increment 36 — проводное доказательство production-заказа (06.10)
## ITEM-508 increment 37 — панель показывает ход заказа (06.10)
## ITEM-509 increment 3 — кириллица и UTF-8 в консоли (07.10)
## ITEM-509 increment 4 — команда `language` (07.10)
## ITEM-509 increment 5 — панель показывает язык (07.10)
## ITEM-509 increment 6 — переключатель языка в панели (07.10)
## ITEM-509 increment 7 — помощь в консоли (509.5) (07.10)
## ITEM-509 increment 8 — рендерер помощи в панели (509.6) (07.10)
## ITEM-509 increment 9 — меню говорит на выбранном языке (509.7b) (07.10)
## ITEM-509 increment 10 — каркас панели и `<html lang>` (509.7a-1) (07.10)
## ITEM-511 — доказательство двух контроллеров не теряет ввод (09.10)
## ITEM-509 increment 27 — язык объявляет носитель (509.7c-2) (09.10)
## ITEM-512 — команды консоли не зависят от регистра (10.10)
## ITEM-513 — кнопка языка в панели, по браузеру (10.10)
## ITEM-514 — выбор режима установки на экране (10.10)
## ITEM-515 — панель: сеть и формы (10.10)
## ITEM-516 — настраиваемый гейт управления (10.10)
## ITEM-517 — установка без UEFI / legacy BIOS (11.10)
## ITEM-518 — диагностика установщика: показать наблюдённое (11.10)
## ITEM-519 — USB-накопитель: транспорт BOT/SCSI (инкремент 1, 11.10)
публикуется в любом случае — `[USB-STORAGE] state=BLOCK_BOUND reason=OK
## ITEM-521 — шов размещения получает доказанный носитель (инкремент 7, 12.10)
## ITEM-521 — доказанный носитель без размещения назван (инкремент 8, 12.10)
## ITEM-521 — журнал называет машину, на которой шёл прогон (инкремент 10, 12.10)
## ITEM-521 — паника называет себя в журнале на носителе (инкремент 11, 12.10)
## ITEM-521 — журнал установки пишется на носитель (инкремент 12, 13.10)
месте записи закрыта ответом владельца — **вариант (в): файл
## ITEM-538 — python-половины проб хранилища и конфигурации читают одно правило (14.10)
## ITEM-539 — python-половины проб appliance/console читают одно правило (14.10)
## ITEM-540 — общий PCI-путь не гадает о портах: фильтр по классу до зондирования BAR (15.10)
## ITEM-541 — страница отказа называет носитель, журнал и хвост кольца (15.10)
и `UNCHECKED reason=NOT_PROBED`.  Страница печатает `[INSTALL-MEDIUM]`,
## ITEM-542 — страница отказа доказана в QEMU (15.10)
## ITEM-543 — BAR, не объяснивший размер, не топит устройство (16.10)
## ITEM-544 — ожидание записи policy boot объявляет свой класс (16.10)
## ITEM-545 — SuperSpeed-носитель: порт получает время, компаньон — в контекст (17.10)
## ITEM-550 — неудавшаяся команда xHCI объясняет себя сама (25.10)
## ITEM-551 — загрузочная флешка видна ядру: USB 2.0 (EHCI) (25.10)
## ITEM-552 — диагностика уезжает файлом, а не фотографией (25.10)
## ITEM-564 — гейт build id читает образ, а не конвейер (28.10)
## ITEM-585 — the local operator is the explicit entropy source on a CPU without RDRAND/RDSEED
## CI-SPEEDUP-3 — the expected refusal is answered, not timed out (04.11)
## CI-SPEEDUP-4 — PBKDF2-HMAC-SHA-256 prepares the key once per derivation (05.11)
## CI-SPEEDUP-5 — the installer proof starts first, and its media are built before the queue (05.11)
## ITEM-588 — обновление принимает исправное состояние по умолчанию и называет отказ (05.11)
## ITEM-589 — PHY RTL8168 на L875: XID 0x2C9 получает свой MII-доступ (05.11)
## ITEM-590 — `diag save` после FULL и `[NET-PHY]` в журнале (05.11)
## ITEM-591 — PHYAR: пауза 20 мкс и фактический advertisement (06.11)
## ITEM-592 — холодный запуск RTL8168evl (XID 0x2C9) по эталону (06.11)
## ITEM-595 — PHYAR: completion poll и отдельное чтение payload (07.11)
Toshiba L875 has `rdrand=0 rdseed=0`, no TPM2 and no platform RNG; the
the ordinary `HOST_PROVISIONED` unlock.
## ITEM-612 — network RCA observability: the frame path in the shared snapshot (15.11)
## ITEM-596 — Запись страницы перед первым PHY-отказом (08.11)
```
