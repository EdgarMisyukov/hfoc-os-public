# ITEM-339 · Release verification / Проверки выпуска

[English](#english) · [Русский](#русский)

HFOC OS `v1.0.0` · `933f7cd739a9437e86638fbfe30a738e22f2e9aa`

## English

### Hardware and existing OS discovery

In-memory MBR and GPT fixtures exercise the `installer_hardware_discovery` report, invalid tables and enforcement of read-only discovery. The result covers the software boundary and synthetic disks.

- `make test-installer-hardware-discovery-host`

[Technical reference](../../hfoc-hardware-os-discovery.md)

## Русский

### Обнаружение оборудования и существующих ОС

Хостовые сценарии с MBR и GPT в памяти проверяют отчёт `installer_hardware_discovery`, обработку повреждённых таблиц и обнаружение в режиме только чтения. Проверка охватывает программный интерфейс и синтетические диски.

- `make test-installer-hardware-discovery-host`

[Техническая документация](../../hfoc-hardware-os-discovery.md)
