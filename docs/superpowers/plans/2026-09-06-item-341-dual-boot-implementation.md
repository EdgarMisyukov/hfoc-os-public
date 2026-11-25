# ITEM-341 · Release verification / Проверки выпуска

[English](#english) · [Русский](#русский)

HFOC OS `v1.0.0` · `933f7cd739a9437e86638fbfe30a738e22f2e9aa`

## English

### Dual-boot layout and menu

The host proof exercises Linux and Windows menu records, preservation of existing partitions, and refusal cases for unsuitable disks or boot paths. Physical dual boot and boot-menu selection require separate hardware evidence.

- `make test-installer-dual-boot-host`

[Technical reference](../../hfoc-dual-boot.md)

## Русский

### Разметка и меню двойной загрузки

Хостовая проверка охватывает записи меню Linux и Windows, сохранение существующих разделов и отказы для неподходящих дисков или цепочек загрузки. Двойная загрузка и выбор пункта меню на реальном оборудовании требуют отдельных результатов приёмки.

- `make test-installer-dual-boot-host`

[Техническая документация](../../hfoc-dual-boot.md)
