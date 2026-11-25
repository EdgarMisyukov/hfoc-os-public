# HFOC OS v1.0.0 release evidence / Свидетельства для выпуска HFOC OS v1.0.0

The following tagged source excerpts support release verification.
Ниже приведены выдержки из тега для проверки выпуска.

| 381 | Hardware acceptance on the dedicated acceptance disk |

## Item ITEM-412

ITEM-399 closes all three:

## Hardware acceptance execution

- ITEM-382 (реализован): licensing surface and notices closure in

- ITEM-383 (реализован): trademark and certified-build policy in

   (она же зеркалится на локальную консоль ITEM-405), ждёт ограниченные

flushed=N` — ровно так, как это уже делал пробник ITEM-410

## The screen the operator reads (ITEM-417)

## The class every proof declares (ITEM-422)

## The menu that offered one system (ITEM-423)

## Item ITEM-434

## Item ITEM-433

## Item ITEM-432

## Item ITEM-431

## Item ITEM-430

## Item ITEM-429

## Item ITEM-428

## Item ITEM-435

## Item ITEM-436

## Item ITEM-437

## Item ITEM-438

## Item ITEM-439

## Item ITEM-440

## Item ITEM-441

## Item ITEM-443

## Item ITEM-442

## Item ITEM-444

## Item ITEM-445

## Item ITEM-446

## Item ITEM-447

## Item ITEM-449

## Item ITEM-451

## Item ITEM-452

## Item ITEM-453

## Item ITEM-454

## Item ITEM-455

## Item ITEM-456

## Item ITEM-457

  нерастущего `max_ms`; обёртка 360 → 480 с по правилу ITEM-427;

## Item ITEM-458

## Item ITEM-459

## Item ITEM-460

## Item ITEM-461

## Item ITEM-462

## Item ITEM-463

## Item ITEM-464

## Item ITEM-465

## Item ITEM-466

## Item ITEM-467

## Item ITEM-469

### Как закрыт ITEM-468

## Item ITEM-470

## Item ITEM-471

## Item ITEM-472

## Item ITEM-473

## Item ITEM-474

## Item ITEM-475

## Item ITEM-476

## Item ITEM-477

## Item ITEM-478

## Item ITEM-479

## Item ITEM-480

## Item ITEM-481

## Item ITEM-482

## Item ITEM-483

## Item ITEM-484

## Item ITEM-485

## Item ITEM-486

## Item ITEM-494

## Item ITEM-495

## Item ITEM-496

## Item ITEM-497

## Item ITEM-498

## Item ITEM-499

## Item ITEM-500

## Item ITEM-501

## Item ITEM-502

## Item ITEM-503

## Item ITEM-504 — фасад: маршруты настраиваются из панели (инкременты 1–10)

мутации `ROUTING_CONFIG` (`110-item-504-routing-mutation.sh`); третий —

## Item ITEM-505

**ITEM-505 — «всё, что настроено в панели, сохраняется в HFOC OS».**  Владелец
снял read-only с панели целиком 2026-09-30 («с панели снять read-only, это

**Инкремент 3 (этот PR) — раздел System настраивает имя хоста.**  Плоскость

## Item ITEM-506

**ITEM-506 — «единая учётная запись: логин и пароль, задаются при установке,
меняются из меню appliance и из удалённой панели».**  Пункт авторизован

**Инкремент 1 (влит) — модель учётной записи и её энергонезависимый

**Инкремент 2 (влит) — владелец учётной записи и разговор консоли.**

**Инкремент 4 (этот PR) — учётная запись задаётся при установке.**

## Item ITEM-510

## Item ITEM-506 — инкремент 6: учётная запись меняется из панели (влито 03.10)

## Item ITEM-507 — SNI и сертификат на имя (влито 04.10; инкременты 1–6)

## Item ITEM-508

### ITEM-508 increment 26 — служба ACME на загрузке: аккаунт и политика продления (05.10)

### ITEM-508 increment 27 — обёртка TCP держит два слушателя (05.10)

### ITEM-508 increment 28 — слушатель HTTP-01 (06.10)

### ITEM-508 increment 29 — ответ HTTP-01 на настоящем проводе (06.10)

### ITEM-508 increment 30 — путь заказа вооружает слушатель (06.10)

### ITEM-508 increment 31 — заказ вооружает слушатель, и ответ уходит на провод (06.10)

### ITEM-508 increment 32 — панель показывает состояние ACME (только чтение) (06.10)

### ITEM-508 increment 33 — проверка RSA для настоящей цепочки Let's Encrypt (06.10)

### ITEM-508 increment 34 — управление ACME: включение, выключение и заявка (06.10)

### ITEM-508 increment 35 — путь заказа на обычной загрузке (06.10)

### ITEM-508 increment 36 — проводное доказательство production-заказа (06.10)

### ITEM-508 increment 37 — панель показывает ход заказа (06.10)

## Item ITEM-509

### ITEM-509 increment 3 — кириллические глифы и UTF-8 в выводе (07.10)

### ITEM-509 increment 4 — команда `language` в консоли (07.10)

### ITEM-509 increment 5 — панель показывает язык (первая половина 509.4) (07.10)

### ITEM-509 increment 6 — переключатель языка в панели (07.10)

### ITEM-509 increment 7 — помощь в консоли (509.5) (07.10)

### ITEM-509 increment 8 — рендерер помощи в панели (509.6) (07.10)

### ITEM-509 increment 9 — меню говорит на выбранном языке (509.7b) (07.10)

### ITEM-509 increment 10 — каркас панели и `<html lang>` (509.7a-1) (07.10)

### ITEM-509 increment 13 — каталог экранов установщика (509.7c-1a) (08.10)

* **Дальше.**  509.7c-1b-2 (экраны дисков и предупреждений), 509.7c-1b-3

### ITEM-509 increment 17 — UTF-8 в пути framebuffer_text (509.7c-1c) (08.10)

### ITEM-509 increment 18 — экраны предупреждений и разделов (509.7c-1b-4) (08.10)

### ITEM-509 increment 19 — баннер установки (509.7c-1b-5) (08.10)

### ITEM-509 increment 20 — разговор учётной записи (509.7d-1) (08.10)

### ITEM-509 increment 21 — подсказки локального меню в консоли (509.7d-2a) (08.10)

### ITEM-509 increment 22 — подписи полей консоли (509.7d-2b) (08.10)

### ITEM-509 increment 23 — проза консоли (509.7d-2c) (09.10)

### ITEM-509 increment 24 — отказ платформы и занятый разговор (509.7d-3a) (09.10)

* **Дальше.**  509.7d-3b — извлекатель и срез-гейт «нет непереведённых строк»

### ITEM-509 increment 25 — проза Help в панели (509.7d-3c-1) (09.10)

### ITEM-509 increment 27 — язык объявляет носитель (509.7c-2) (09.10)

## Item ITEM-512

**ITEM-512 — «команды консоли не зависят от регистра».**  Единственный
авторизованный пункт: он заведён по standing-разрешению владельца 2026-09-16

`test-usb-two-controllers-qemu` (ITEM-511) на прогоне PR #668: проба будит

## Item ITEM-513

**ITEM-513 — «кнопка языка в панели, по браузеру».**  Единственный
авторизованный пункт: решение владельца 2026-10-10 03:05 UTC

## Item ITEM-514

## Item ITEM-515

**ITEM-515 — «панель: сеть и формы» (находка F-3).**  Авторизовано владельцем

## Item ITEM-516

**ITEM-516 — «настраиваемый гейт управления» (пункт 5).**  Авторизовано

## Item ITEM-517

**ITEM-517 — «установка без UEFI / legacy BIOS» (пункт 4).**  Авторизовано

## Item ITEM-518

**ITEM-518 — «диагностика установщика: показать наблюдённое» (пункт 6).**

## Item ITEM-519

**ITEM-519 (инкремент 5) — «носитель называет себя загрузочным».**

## Item ITEM-521 — инкремент 1: ядро журнала не зависит от размещения (влито 12.10)

## Item ITEM-521 — инкремент 3: JSON-половина читалки закреплена свидетелем

## Item ITEM-521 — инкремент 4: документированные пути читалки закреплены свидетелем

## Item ITEM-521 — инкремент 5: журнал подключён к воронке

## Item ITEM-521 — инкремент 6: носитель только для чтения назван

## Item ITEM-521 — инкремент 7: шов размещения получает доказанный носитель (влито 12.10)

## Item ITEM-521 — инкремент 8: доказанный носитель без размещения назван (влито 12.10)

## Item ITEM-521 — инкремент 9: журнал не пишет за предел, который объявило размещение (влито 12.10)

## Item ITEM-521

**ITEM-521 (инкремент 11, fork-independent) — «паника называет себя в журнале на носителе».**

  ответе на развилку ITEM-520.

## Item ITEM-526

## Item ITEM-527

## Item ITEM-528

## Item ITEM-529

## Item ITEM-530

## Item ITEM-531

## Item ITEM-532

## Item ITEM-533

## Item ITEM-534

## Item ITEM-535

## Item ITEM-536

## Item ITEM-537

## Item ITEM-521 (инкремент 12)

**ITEM-521 (инкремент 12, размещение) — «журнал установки пишется на носитель»:
развилка ITEM-520 закрыта ответом владельца, вариант (в).**

## Item ITEM-538

## Item ITEM-539

## Item ITEM-540

## Item ITEM-541

## Item ITEM-542

## Item ITEM-543

## Item ITEM-544

## Item ITEM-545

**ITEM-545 — «SuperSpeed-носитель: порт получает время, компаньон — в
контекст, привязка — в QEMU» (пункт 10 живой очереди, улика с железа

## Item ITEM-547

**ITEM-547 — «Установленный BIOS-диск помечен активным, как помечает носитель
Limine» (улика с железа 2026-10-17).**

## Item ITEM-564

**ITEM-564 — два пункта, влитых под одним номером: гейт build id (PR #734) и
два вердикта передачи/остановки (PR #735).**

Номер `ITEM-564` был выдан дважды.  Сначала им назвали гейт build id

## Item ITEM-565

## Item ITEM-566

## Item ITEM-582

## Item ITEM-583

## Item ITEM-584

## Item ITEM-585

## Item ITEM-586

## Item ITEM-590

## Item ITEM-595 — PHYAR payload after completion (software merged; hardware OPEN)

## Item ITEM-596 — Page write evidence (software merged; hardware OPEN)

## Item ITEM-597

## Item ITEM-598 — host-provisioned entropy (merged as PR777)

## Item ITEM-600 — STATIC boot ordering (merged as PR778)

## Item ITEM-601 — SSH Task 2 endpoint (merged as PR779)

## Item ITEM-602 — SSH Task 3 (merged as PR780)

## Item ITEM-603 — SSH Task 4a (merged as PR781)

## Item ITEM-604 — SSH Task 4b (merged as PR782)

## Item ITEM-605 — production lwIP timeout servicing (merged as PR783)

## Item ITEM-606 — owner-confirmed SSH public-key import (merged as PR784)

## Item ITEM-607 — SSH secret lifetime and revoke semantics (merged as PR785)

## Item ITEM-608 — final delivery medium record and owner handoff

## Item ITEM-609 — update-proof diagnosis and owner-key delivery (merged as PR787)

## Item ITEM-610 — readable local login and larger console text (merged as PR788)

## Item ITEM-611 — local login latency: measure and fix (merged as PR789)

## Item ITEM-613

## Item ITEM-612

## Item ITEM-615

## Item ITEM-616

## Item ITEM-617

## Item ITEM-618

## Item ITEM-619

## Item ITEM-621

## Item ITEM-624

## Item ITEM-625

## Item ITEM-634

## Item ITEM-637

## Item ITEM-639

## Item ITEM-640

**ITEM-640 — the physical pointer's 16-bit axes are decoded, not truncated.**

The 2026-11-21 Toshiba diagnostic on `eca0e3fd06e0` read the 71-byte report

## Item ITEM-641

**ITEM-641 — a proof reads a kernel marker in one process.**

`make test-diag-failure-qemu` refused a production-installer kernel that did

## Item ITEM-642

**ITEM-642 — the release gate starts from a clean build.**

The 1.0.0 preparation on `d385d6aa` showed the first

## Item ITEM-643

**ITEM-643 — the receive path publishes a live outage snapshot.**

## Item ITEM-644

**ITEM-644 — a failed backend frees the relay slot.**

The 2026-11-22 backend-unavailable trial on Toshiba `d471b4b2b3c1` left the

## Item ITEM-645

**ITEM-645 — the poll loop acknowledges a stopped receiver.**

## Item ITEM-646

## Item ITEM-647

## Item ITEM-648

## Item ITEM-649

## Item ITEM-650

## Item ITEM-651

## Item ITEM-652

## Item ITEM-653

## Item ITEM-654

## Item ITEM-655

## Item ITEM-656

## Item ITEM-657

## Item ITEM-658

## Item ITEM-659

## Item ITEM-660

## Item ITEM-661

## Current item

**ITEM-662 — the installed boot menu names the release version.**
