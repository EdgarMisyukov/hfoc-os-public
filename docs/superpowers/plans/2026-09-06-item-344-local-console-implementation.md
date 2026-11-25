# ITEM-344 · Release verification / Проверки выпуска

[English](#english) · [Русский](#русский)

HFOC OS `v1.0.0` · `933f7cd739a9437e86638fbfe30a738e22f2e9aa`

## English

### Local appliance console

Host fixtures cover the snapshot and action vocabulary. A QEMU guest boots the production ISO and exercises the local console output. The proofs apply to the software console; hardware input support has its own acceptance criteria.

- `make test-appliance-boot-console-host`
- `make test-appliance-boot-console-qemu`

[Technical reference](../../hfoc-appliance-boot-console.md)

## Русский

### Локальная консоль системы

Хостовые сценарии проверяют снимок состояния и набор действий. В QEMU производственный ISO загружается и проверяется вывод локальной консоли. Эти проверки охватывают программную консоль. Для средств ввода на реальном оборудовании действуют отдельные критерии приёмки.

- `make test-appliance-boot-console-host`
- `make test-appliance-boot-console-qemu`

[Техническая документация](../../hfoc-appliance-boot-console.md)
