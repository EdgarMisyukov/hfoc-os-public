# ITEM-342 · Release verification / Проверки выпуска

[English](#english) · [Русский](#русский)

HFOC OS `v1.0.0` · `933f7cd739a9437e86638fbfe30a738e22f2e9aa`

## English

### Bootloader recovery planning

Host fixtures cover damaged bootloader state, missing EFI entries, unconfirmed plans, and incomplete repair outcomes. This evidence covers decisions and validation in memory. Physical repair requires separate hardware evidence.

- `make test-installer-boot-recovery-host`

[Technical reference](../../hfoc-bootloader-recovery.md)

## Русский

### Планирование восстановления загрузчика

Хостовые сценарии охватывают повреждение загрузчика, отсутствие записей EFI, неподтверждённые планы и неполные результаты ремонта. Они проверяют принятие решений и проверку результата в памяти. Восстановление на реальном оборудовании требует отдельных результатов приёмки.

- `make test-installer-boot-recovery-host`

[Техническая документация](../../hfoc-bootloader-recovery.md)
