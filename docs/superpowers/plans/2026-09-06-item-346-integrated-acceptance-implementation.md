# ITEM-346 · Release verification / Проверки выпуска

[English](#english) · [Русский](#русский)

HFOC OS `v1.0.0` · `933f7cd739a9437e86638fbfe30a738e22f2e9aa`

## English

### Integrated installer and management acceptance

The gate verifies the matrix’s exact categories, target names and published acceptance markers. The host target tests the gate; the acceptance target runs the real software gate.

- `make test-hfoc-integrated-installer-dual-boot-management-acceptance-host`
- `make test-hfoc-integrated-installer-dual-boot-management-acceptance`

[Technical reference](../../hfoc-integrated-installer-dual-boot-management-acceptance.md)

## Русский

### Комплексная приёмка установщика и управления

Проверка сверяет точные категории матрицы, имена целей и маркеры результатов. Хостовая цель проверяет сам механизм, а приёмочная цель запускает программную проверку по матрице.

- `make test-hfoc-integrated-installer-dual-boot-management-acceptance-host`
- `make test-hfoc-integrated-installer-dual-boot-management-acceptance`

[Техническая документация](../../hfoc-integrated-installer-dual-boot-management-acceptance.md)
