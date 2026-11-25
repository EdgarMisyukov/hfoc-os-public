# ITEM-340 · Release verification / Проверки выпуска

[English](#english) · [Русский](#русский)

HFOC OS `v1.0.0` · `933f7cd739a9437e86638fbfe30a738e22f2e9aa`

## English

### Clean installer layout

Host fixtures exercise unconfirmed and confirmed plans, GPT rescan, and rollback after an injected write failure. This check covers the layout transaction on in-memory storage.

- `make test-installer-layout-host`

[Technical reference](../../hfoc-installer-layout.md)

## Русский

### Разметка при чистой установке

Хостовые сценарии проверяют неподтверждённые и подтверждённые планы, повторное чтение GPT и откат после искусственного сбоя записи. Проверка охватывает транзакцию разметки на диске в памяти.

- `make test-installer-layout-host`

[Техническая документация](../../hfoc-installer-layout.md)
